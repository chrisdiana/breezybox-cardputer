#include "breezy_vfs.h"
#include "esp_littlefs.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "breezy_vfs";
static char s_cwd[BREEZYBOX_MAX_PATH + 1] = BREEZYBOX_MOUNT_POINT;
static bool s_sd_mounted;
static sdmmc_card_t *s_sd_card;
static breezybox_root_fs_kind_t s_root_fs_kind = BREEZYBOX_ROOT_FS_NONE;

#if defined(BREEZY_BOARD_STICKS3)
static void try_mount_sd_card(void)
{
    ESP_LOGI(TAG, "SD card mounting is not configured for StickS3");
}
#else
#define CARDPUTER_SD_CLK_PIN  GPIO_NUM_40
#define CARDPUTER_SD_MOSI_PIN GPIO_NUM_14
#define CARDPUTER_SD_MISO_PIN GPIO_NUM_39
#define CARDPUTER_SD_CS_PIN   GPIO_NUM_12
#define CARDPUTER_SD_HOST     SPI3_HOST

static void try_mount_sd_card(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CARDPUTER_SD_MOSI_PIN,
        .miso_io_num = CARDPUTER_SD_MISO_PIN,
        .sclk_io_num = CARDPUTER_SD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(CARDPUTER_SD_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SD SPI bus init failed: %s", esp_err_to_name(ret));
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = CARDPUTER_SD_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CARDPUTER_SD_CS_PIN;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    ret = esp_vfs_fat_sdspi_mount(BREEZYBOX_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_sd_card);
    if (ret == ESP_OK) {
        s_sd_mounted = true;
        ESP_LOGI(TAG, "SD card mounted at %s", BREEZYBOX_SD_MOUNT_POINT);
        return;
    }

    if (ret == ESP_FAIL || ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_INVALID_RESPONSE || ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "SD card not mounted");
    } else {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(ret));
    }
}
#endif

bool breezybox_sd_mounted(void)
{
    return s_sd_mounted;
}

breezybox_root_fs_kind_t breezybox_root_fs_kind(void)
{
    return s_root_fs_kind;
}

const char *breezybox_root_fs_name(void)
{
    switch (s_root_fs_kind) {
        case BREEZYBOX_ROOT_FS_LITTLEFS:
            return "littlefs";
        case BREEZYBOX_ROOT_FS_FAT:
            return "fat";
        case BREEZYBOX_ROOT_FS_SPIFFS:
            return "spiffs";
        default:
            return "none";
    }
}

esp_err_t breezybox_root_fs_info(size_t *total_bytes, size_t *used_bytes)
{
    if (!total_bytes || !used_bytes) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (s_root_fs_kind) {
        case BREEZYBOX_ROOT_FS_LITTLEFS:
            return esp_littlefs_info("storage", total_bytes, used_bytes);
        case BREEZYBOX_ROOT_FS_FAT: {
            uint64_t fat_total = 0;
            uint64_t fat_free = 0;
            esp_err_t err = esp_vfs_fat_info(BREEZYBOX_MOUNT_POINT, &fat_total, &fat_free);
            if (err != ESP_OK) {
                return err;
            }
            *total_bytes = (size_t)fat_total;
            *used_bytes = (size_t)(fat_total - fat_free);
            return ESP_OK;
        }
        case BREEZYBOX_ROOT_FS_SPIFFS:
            return esp_spiffs_info("spiffs", total_bytes, used_bytes);
        default:
            return ESP_ERR_INVALID_STATE;
    }
}

void breezybox_get_cwd(char *buf, size_t size)
{
    if (size > 0) {
        strncpy(buf, s_cwd, size - 1);
        buf[size - 1] = '\0';
    }
}

const char *breezybox_cwd(void)
{
    return s_cwd;
}

int breezybox_set_cwd(const char *path)
{
    char new_path[BREEZYBOX_MAX_PATH + 1];

    if (strcmp(path, "..") == 0) {
        if (strcmp(s_cwd, "/") == 0) return 0;
        strncpy(new_path, s_cwd, sizeof(new_path));
        char *last_slash = strrchr(new_path, '/');
        if (last_slash == new_path) {
            new_path[1] = '\0';
        } else if (last_slash) {
            *last_slash = '\0';
        }
    } else if (path[0] == '/') {
        strncpy(new_path, path, sizeof(new_path) - 1);
        new_path[sizeof(new_path) - 1] = '\0';
    } else {
        size_t cwd_len = strlen(s_cwd);
        size_t path_len = strlen(path);
        if (cwd_len + 1 + path_len >= sizeof(new_path)) return -1;

        strcpy(new_path, s_cwd);
        if (cwd_len > 1) {
            new_path[cwd_len] = '/';
            strcpy(new_path + cwd_len + 1, path);
        } else {
            strcpy(new_path + 1, path);
        }
    }

    struct stat st;
    if (strcmp(new_path, "/") == 0 ||
        (stat(new_path, &st) == 0 && S_ISDIR(st.st_mode))) {
        strcpy(s_cwd, new_path);
        return 0;
    }
    return -1;
}

char *breezybox_resolve_path(const char *path, char *buf, size_t size)
{
    if (path[0] == '/') {
        strncpy(buf, path, size - 1);
        buf[size - 1] = '\0';
    } else {
        size_t cwd_len = strlen(s_cwd);
        size_t path_len = strlen(path);
        if (cwd_len + 1 + path_len >= size) return NULL;
        
        if (cwd_len == 1 && s_cwd[0] == '/') {
            snprintf(buf, size, "/%s", path);
        } else {
            snprintf(buf, size, "%s/%s", s_cwd, path);
        }
    }
    return buf;
}

static esp_err_t try_mount_littlefs_root(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = BREEZYBOX_MOUNT_POINT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret == ESP_OK) {
        s_root_fs_kind = BREEZYBOX_ROOT_FS_LITTLEFS;
    }
    return ret;
}

static esp_err_t try_mount_spiffs_root(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = BREEZYBOX_MOUNT_POINT,
        .partition_label = "spiffs",
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK) {
        s_root_fs_kind = BREEZYBOX_ROOT_FS_SPIFFS;
    }
    return ret;
}

static esp_err_t try_mount_fat_root(void)
{
    esp_vfs_fat_mount_config_t conf = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(
        BREEZYBOX_MOUNT_POINT,
        "vfs",
        &conf,
        &wl_handle
    );
    if (ret == ESP_OK) {
        s_root_fs_kind = BREEZYBOX_ROOT_FS_FAT;
    }
    return ret;
}

esp_err_t breezybox_vfs_init(void)
{
    s_sd_mounted = false;
    s_sd_card = NULL;
    s_root_fs_kind = BREEZYBOX_ROOT_FS_NONE;

    esp_err_t ret = try_mount_littlefs_root();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS root mount failed: %s", esp_err_to_name(ret));

        ret = try_mount_fat_root();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "FAT root mount failed: %s", esp_err_to_name(ret));

            ret = try_mount_spiffs_root();
            if (ret != ESP_OK) {
                if (ret == ESP_FAIL) {
                    printf("Failed to mount or format internal filesystem\n");
                } else if (ret == ESP_ERR_NOT_FOUND) {
                    printf("No compatible internal filesystem partition found\n");
                }
                return ret;
            }

            ESP_LOGI(TAG, "Mounted %s from SPIFFS partition", BREEZYBOX_MOUNT_POINT);
        } else {
            ESP_LOGI(TAG, "Mounted %s from FAT partition", BREEZYBOX_MOUNT_POINT);
        }
    }

    try_mount_sd_card();
    strcpy(s_cwd, BREEZYBOX_MOUNT_POINT);
    return ESP_OK;
}

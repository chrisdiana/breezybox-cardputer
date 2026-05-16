#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define BREEZYBOX_MAX_PATH 128
#define BREEZYBOX_MOUNT_POINT "/root"
#define BREEZYBOX_SD_MOUNT_POINT "/sd"

/**
 * @brief Initialize BreezyBox filesystem (LittleFS on internal flash)
 * @return ESP_OK on success
 */
esp_err_t breezybox_vfs_init(void);

/**
 * @brief Check whether the SD card filesystem is mounted.
 */
bool breezybox_sd_mounted(void);

/**
 * @brief Get current working directory
 */
void breezybox_get_cwd(char *buf, size_t size);

/**
 * @brief Get pointer to current working directory (internal use)
 */
const char *breezybox_cwd(void);

/**
 * @brief Set current working directory
 * @return 0 on success, -1 on failure
 */
int breezybox_set_cwd(const char *path);

/**
 * @brief Resolve a path (relative or absolute) to full path
 * @return pointer to buf on success, NULL on failure
 */
char *breezybox_resolve_path(const char *path, char *buf, size_t size);

/*
 * df.c - Show disk free space
 * 
 * Usage: df
 */

#include <stdio.h>
#include "esp_vfs_fat.h"
#include "esp_littlefs.h"
#include "breezy_vfs.h"

static void print_fs_row(const char *mount_point, unsigned long total_kb, unsigned long used_kb)
{
    unsigned long free_kb = total_kb - used_kb;
    int percent = (total_kb > 0) ? ((int)(used_kb * 100 / total_kb)) : 0;

    printf("%-12s  %5luK  %5luK  %5luK  %3d%%\n",
           mount_point, total_kb, used_kb, free_kb, percent);
}

int cmd_df(int argc, char **argv)
{
    (void)argc; (void)argv;
    
    size_t total_bytes = 0, used_bytes = 0;
    
    // Get LittleFS partition info
    // The partition label is "storage" as configured in breezy_vfs.c
    esp_err_t ret = esp_littlefs_info("storage", &total_bytes, &used_bytes);
    if (ret != ESP_OK) {
        printf("df: cannot get filesystem info\n");
        return 1;
    }
    
    printf("Filesystem      Size    Used   Avail  Use%%\n");
    print_fs_row(BREEZYBOX_MOUNT_POINT, total_bytes / 1024, used_bytes / 1024);

    if (breezybox_sd_mounted()) {
        uint64_t sd_total_bytes = 0;
        uint64_t sd_free_bytes = 0;
        if (esp_vfs_fat_info(BREEZYBOX_SD_MOUNT_POINT, &sd_total_bytes, &sd_free_bytes) == ESP_OK) {
            unsigned long sd_total_kb = (unsigned long)(sd_total_bytes / 1024);
            unsigned long sd_used_kb = (unsigned long)((sd_total_bytes - sd_free_bytes) / 1024);
            print_fs_row(BREEZYBOX_SD_MOUNT_POINT, sd_total_kb, sd_used_kb);
        }
    }
    
    return 0;
}

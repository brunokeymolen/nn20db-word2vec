/*
 * sd_storage.c - SDMMC storage driver hooks for nn20db.
 *
 * Provides the board-specific SDMMC mount/unmount implementation used by
 * nn20db and a small directory listing helper for diagnostics.
 */

#include "sd_storage.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "platform/nn20db_esp32_storage.h"

static const char *TAG = "sd_storage";

/* SD card SDMMC pins - Waveshare ESP32-S3-Touch-LCD-1.47 (from official BSP) */
#define PIN_SD_CLK      16
#define PIN_SD_CMD      15
#define PIN_SD_D0       17
#define PIN_SD_D1       18
#define PIN_SD_D2       13
#define PIN_SD_D3       14

/*
 * nn20db calls these hooks when it needs to mount/unmount storage.
 * We provide the correct 4-bit SDMMC pins for the Waveshare board so the
 * library never falls back to its built-in defaults (CLK=39/CMD=38/D0=40).
 */
static int sd_storage_mount(const char *mount_point, void **out_handle)
{
    ESP_LOGI(TAG, "Mounting SD card at %s ...", mount_point);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_cfg = {};
    slot_cfg.clk   = PIN_SD_CLK;
    slot_cfg.cmd   = PIN_SD_CMD;
    slot_cfg.d0    = PIN_SD_D0;
    slot_cfg.d1    = PIN_SD_D1;
    slot_cfg.d2    = PIN_SD_D2;
    slot_cfg.d3    = PIN_SD_D3;
    slot_cfg.width = 4;
    slot_cfg.cd    = SDMMC_SLOT_NO_CD;
    slot_cfg.wp    = SDMMC_SLOT_NO_WP;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 24,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_cfg,
                                             &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "SD mounted OK (%s, %llu MB)",
             card->cid.name,
             (unsigned long long)((uint64_t)card->csd.capacity *
                                  card->csd.sector_size / (1024 * 1024)));
    *out_handle = card;
    return 0;
}

static void sd_storage_unmount(const char *mount_point, void *handle)
{
    esp_vfs_fat_sdcard_unmount(mount_point, (sdmmc_card_t *)handle);
}

static const nn20db_esp32_storage_driver_t s_sd_driver = {
    .mount   = sd_storage_mount,
    .unmount = sd_storage_unmount,
};

void sd_storage_register_driver(void)
{
    nn20db_esp32_set_storage_driver(&s_sd_driver);
}

static void sd_storage_list_dir(const char *path, int depth)
{
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "  [cannot open %s]", path);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (ent->d_type == DT_DIR) {
            ESP_LOGI(TAG, "%*s[%s/]", depth * 2, "", ent->d_name);
            sd_storage_list_dir(child, depth + 1);
        } else {
            ESP_LOGI(TAG, "%*s%s", depth * 2, "", ent->d_name);
        }
    }
    closedir(dir);
}

void sd_storage_log_contents(const char *mount_point)
{
    ESP_LOGI(TAG, "--- SD card contents (%s) ---", mount_point);
    sd_storage_list_dir(mount_point, 0);
    ESP_LOGI(TAG, "--- end of SD listing ---");
}

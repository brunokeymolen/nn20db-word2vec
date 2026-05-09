/*
 * main.c — Word2Vec search server for ESP32-S3
 *
 * Boot sequence:
 *   1. Mount SD card (FAT, /sdcard).
 *   2. Initialise LVGL display (idle screen, "Wi-Fi: connecting...").
 *   3. Connect to Wi-Fi, update display with IP address.
 *   4. Open nn20db database from SD card.
 *   5. Start TCP server task (net_server_start).
 *   6. Display "Waiting for query...".
 *
 * The TCP server (net_server.c) handles incoming search queries and
 * calls display_show_results() after each successful search.
 *
 * Phase 1: search-only. The database is built on Linux and copied to
 *           the SD card at /nand0/word2vec/ (8.3 path format).
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "nn20db.h"
#include "nn20db_config.h"
#include "platform/nn20db_esp32_storage.h"

#include "net_server.h"
#include "display.h"

static const char *TAG = "main";

/* ── Configuration ──────────────────────────────────────────────────────── */
/*
 * Set your Wi-Fi credentials via idf.py menuconfig:
 *   Word2Vec Search Configuration → Wi-Fi SSID / Password
 * Or override the defaults below for quick development builds.
 */
#ifndef CONFIG_WORD2VEC_WIFI_SSID
#define CONFIG_WORD2VEC_WIFI_SSID     "myssid"
#endif
#ifndef CONFIG_WORD2VEC_WIFI_PASSWORD
#define CONFIG_WORD2VEC_WIFI_PASSWORD "mypassword"
#endif

/* SD card SDMMC pins — Waveshare ESP32-S3-Touch-LCD-1.47 (from official BSP) */
#define PIN_SD_CLK      16
#define PIN_SD_CMD      15
#define PIN_SD_D0       17
#define PIN_SD_D1       18
#define PIN_SD_D2       13
#define PIN_SD_D3       14

/* nn20db database path (8.3 format, on mounted FAT) */
#define DB_PATH         "/sdcard/nand0/word2vec"

/* ── nn20db config ──────────────────────────────────────────────────────── */

static const nn20db_config s_nn20db_config = {
    .vector = {
        .type          = NN20DB_DIMENSION_FLOAT32_CONFIG,
        .dimension     = NET_SERVER_DIM,
        .metadata_size = 32,
    },
    .storage = {
        .type = NN20DB_STORAGE_LFS_CONFIG,
        .lfs = {
            .device_path             = DB_PATH,
            .mount_point             = "/sdcard",
            .lane_cache_size_kb      = 16,
            .lane_size_mb            = 512,
            .log_size_mb             = 4,
            .log_index_buckets       = 1024,
            .object_cache_size_bytes = 4096,
            .read_ahead_size_bytes   = 2048,
            .block_size              = 4096,
            .flags                   = NN20DB_STORAGE_FLAGS_DISABLE_CRC,
        },
        .cache = {
            .enabled     = 1,
            .max_entries = 16,  /* 16 × 4096 = 64 KB — safe for S3 PSRAM */
        },
    },
    .metric = {
        .type = METRIC_EUCLIDEAN_F32_CONFIG,
    },
};

/* ── Wi-Fi ──────────────────────────────────────────────────────────────── */

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_RETRY_BIT     BIT2
#define WIFI_MAX_RETRIES   10
#define WIFI_RETRY_DELAY_MS 750

static int s_wifi_retries = 0;
static int s_wifi_last_disconnect_reason = 0;
static char s_ip_str[32] = "0.0.0.0";

static bool wifi_reason_needs_restart(int reason)
{
    return reason == WIFI_REASON_AUTH_EXPIRE ||
           reason == WIFI_REASON_ASSOC_LEAVE ||
           reason == WIFI_REASON_CONNECTION_FAIL;
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_last_disconnect_reason = disc->reason;
        if (s_wifi_retries < WIFI_MAX_RETRIES) {
            s_wifi_retries++;
            ESP_LOGW(TAG, "Wi-Fi retry %d (reason %d)", s_wifi_retries, disc->reason);
            xEventGroupSetBits(s_wifi_events, WIFI_RETRY_BIT);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        s_wifi_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(void) {
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    /* Use RAM storage only — prevents stale NVS config from overriding our
       settings on power cycle (works-after-flash / fails-after-replug issue) */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &h2));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid,     CONFIG_WORD2VEC_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_WORD2VEC_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable    = true;
    wifi_cfg.sta.pmf_cfg.required   = false;
    /* listen_interval=1: advertise to the AP that we never sleep between
       beacons; range extenders drop stations with li>1 (reason 8) */
    wifi_cfg.sta.listen_interval    = 1;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Disable power-save mode — prevents reason-8 (ASSOC_LEAVE) disconnects
       caused by the radio sleeping during the initial association handshake. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_connect());

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(20000);
    while (1) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait_ticks = (deadline > now) ? (deadline - now) : 0;
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_RETRY_BIT,
            pdTRUE,
            pdFALSE,
            wait_ticks);

        if (bits & WIFI_CONNECTED_BIT) {
            return true;
        }
        if ((bits & WIFI_FAIL_BIT) || wait_ticks == 0) {
            return false;
        }
        if (bits & WIFI_RETRY_BIT) {
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
            if (wifi_reason_needs_restart(s_wifi_last_disconnect_reason)) {
                ESP_LOGI(TAG, "Restarting Wi-Fi after disconnect reason %d",
                         s_wifi_last_disconnect_reason);
                ESP_ERROR_CHECK(esp_wifi_stop());
                vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
                ESP_ERROR_CHECK(esp_wifi_start());
                ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
                ESP_ERROR_CHECK(esp_wifi_connect());
            } else {
                ESP_ERROR_CHECK(esp_wifi_connect());
            }
        }
    }
}

/* ── SD card storage driver (custom vtable for nn20db) ─────────────────── */
/*
 * nn20db calls these hooks when it needs to mount/unmount storage.
 * We provide the correct 4-bit SDMMC pins for the Waveshare board so the
 * library never falls back to its built-in defaults (CLK=39/CMD=38/D0=40).
 */

static int my_sd_mount(const char *mount_point, void **out_handle) {
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
        .max_files              = 8,
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

static void my_sd_unmount(const char *mount_point, void *handle) {
    esp_vfs_fat_sdcard_unmount(mount_point, (sdmmc_card_t *)handle);
}

static const nn20db_esp32_storage_driver_t s_sd_driver = {
    .mount   = my_sd_mount,
    .unmount = my_sd_unmount,
};

/* ── SD directory listing ───────────────────────────────────────────────── */

static void sd_list_dir(const char *path, int depth) {
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
            sd_list_dir(child, depth + 1);
        } else {
            ESP_LOGI(TAG, "%*s%s", depth * 2, "", ent->d_name);
        }
    }
    closedir(dir);
}

static void sd_log_contents(const char *mount_point) {
    ESP_LOGI(TAG, "--- SD card contents (%s) ---", mount_point);
    sd_list_dir(mount_point, 0);
    ESP_LOGI(TAG, "--- end of SD listing ---");
}

/* ── app_main ───────────────────────────────────────────────────────────── */

void app_main(void) {
    /* NVS (required by Wi-Fi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* display init (shows idle screen immediately) */
    display_init();

    /* Register custom SD storage driver for nn20db (must be before any nn20db call) */
    nn20db_esp32_set_storage_driver(&s_sd_driver);

    /* Wi-Fi */
    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s", CONFIG_WORD2VEC_WIFI_SSID);
    if (wifi_connect()) {
        ESP_LOGI(TAG, "Wi-Fi connected, IP: %s", s_ip_str);
        display_set_ip(s_ip_str);
    } else {
        ESP_LOGW(TAG, "Wi-Fi connection failed — running without network");
        display_set_ip("offline");
    }

    /* open nn20db */
    ESP_LOGI(TAG, "Opening nn20db at %s ...", DB_PATH);
    NN20DB *db = NULL;
    int rc = nn20db_open_with_config(&s_nn20db_config, &db);
    if (rc != NN20DB_ERROR_OK || db == NULL) {
        ESP_LOGE(TAG, "Failed to open DB (rc=%d)", rc);
        /* display error and halt */
        display_set_ip("DB open failed!");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "nn20db opened OK");

    /* Log SD card contents for diagnostics */
    sd_log_contents("/sdcard");

    /* start TCP search server */
    net_server_start(db);
    ESP_LOGI(TAG, "TCP server started on port %d", NET_SERVER_PORT);

    /* idle — the server task drives further activity */
    ESP_LOGI(TAG, "Ready. Waiting for queries on %s:%d",
             s_ip_str, NET_SERVER_PORT);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

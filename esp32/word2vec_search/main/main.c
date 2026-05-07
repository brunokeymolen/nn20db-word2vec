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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "nn20db.h"
#include "nn20db_config.h"

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
#define CONFIG_WORD2VEC_WIFI_SSID     "your_ssid"
#endif
#ifndef CONFIG_WORD2VEC_WIFI_PASSWORD
#define CONFIG_WORD2VEC_WIFI_PASSWORD "your_password"
#endif

/* SD card SPI pins — Waveshare ESP32-S3-Touch-LCD-1.47 */
#define SD_SPI_HOST     SPI3_HOST
#define PIN_SD_MOSI     35
#define PIN_SD_MISO     36
#define PIN_SD_CLK      37
#define PIN_SD_CS       34

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
#define WIFI_MAX_RETRIES   10

static int s_wifi_retries = 0;
static char s_ip_str[32] = "0.0.0.0";

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retries < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_wifi_retries++;
            ESP_LOGW(TAG, "Wi-Fi retry %d", s_wifi_retries);
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(20000));

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ── SD card mount ──────────────────────────────────────────────────────── */

static void sd_mount(void) {
    ESP_LOGI(TAG, "Mounting SD card ...");

    spi_bus_config_t buscfg = {
        .mosi_io_num   = PIN_SD_MOSI,
        .miso_io_num   = PIN_SD_MISO,
        .sclk_io_num   = PIN_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs   = PIN_SD_CS;
    slot_cfg.host_id   = SD_SPI_HOST;

    sdmmc_host_t host       = SDSPI_HOST_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_card_t *card;
    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg,
                                             &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        /* Halt — database is on SD card, nothing useful without it. */
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "SD mounted OK (%s, %llu MB)",
             card->cid.name,
             (unsigned long long)((uint64_t)card->csd.capacity *
                                  card->csd.sector_size / (1024 * 1024)));
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

    /* SD card */
    sd_mount();

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

    /* start TCP search server */
    net_server_start(db);
    ESP_LOGI(TAG, "TCP server started on port %d", NET_SERVER_PORT);

    /* idle — the server task drives further activity */
    ESP_LOGI(TAG, "Ready. Waiting for queries on %s:%d",
             s_ip_str, NET_SERVER_PORT);
}

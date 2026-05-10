/*
 * nn20db-word2vec
 *
 * Copyright (c) 2026 Bruno Keymolen
 * Contact: bruno.keymolen@gmail.com
 *
 * License:
 * This Demo, including all pre-compiled binaries and accompanying files,
 * is provided for private and educational use only.
 *
 * Commercial use is strictly prohibited without prior written agreement
 * from the author.
 *
 * Disclaimer:
 * This software is provided "as is", without any express or implied
 * warranties, including but not limited to the implied warranties of
 * merchantability and fitness for a particular purpose.
 *
 * In no event shall the author be held liable for any damages arising
 * from the use of this software.
 */

/*
 * main.c — Word2Vec search server for ESP32-S3
 *
 * Boot sequence:
 *   1. Initialise LVGL display at low backlight for immediate feedback.
 *   2. Connect to Wi-Fi, then raise the LCD backlight.
 *   3. Mount/open database on SD card.
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
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs.h"

#include "nn20db.h"
#include "nn20db_config.h"

#include "net_server.h"
#include "display.h"
#include "sd_storage.h"

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

/* nn20db database path (8.3 format, on mounted FAT) */
//#define DB_PATH         "/sdcard/nand0/word2vec"
#define DB_PATH         "/sdcard/nand0/w2v3m"

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
#define WIFI_MAX_RETRIES   30
#define WIFI_RETRY_DELAY_MS 1500
#define WIFI_CONNECT_TIMEOUT_MS 60000
#define WIFI_SCAN_MAX_APS 20
#define LCD_BOOT_BACKLIGHT_PERCENT 25
#define LCD_WIFI_BACKLIGHT_PERCENT 100

static int s_wifi_retries = 0;
static int s_wifi_last_disconnect_reason = 0;
static int s_wifi_last_scan_rssi = -127;
static int s_wifi_last_scan_count = 0;
static char s_ip_str[32] = "0.0.0.0";
static nvs_handle_t s_diag_nvs = 0;
static uint32_t s_boot_count = 0;

typedef enum {
    BOOT_PHASE_START = 1,
    BOOT_PHASE_NVS_READY,
    BOOT_PHASE_WIFI_INIT,
    BOOT_PHASE_WIFI_SCAN,
    BOOT_PHASE_WIFI_CONNECTING,
    BOOT_PHASE_WIFI_CONNECTED,
    BOOT_PHASE_WIFI_FAILED,
    BOOT_PHASE_DISPLAY_INIT,
    BOOT_PHASE_DB_OPENING,
    BOOT_PHASE_READY,
} boot_phase_t;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
    }
}

static const char *wifi_reason_name(int reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:       return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE:       return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:        return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE:      return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY:     return "ASSOC_TOOMANY";
    case WIFI_REASON_NOT_AUTHED:        return "NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED:       return "NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE:       return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED:  return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:
        return "DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
        return "DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_BSS_TRANSITION_DISASSOC:
        return "BSS_TRANSITION_DISASSOC";
    case WIFI_REASON_IE_INVALID:        return "IE_INVALID";
    case WIFI_REASON_MIC_FAILURE:       return "MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        return "IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_GROUP_CIPHER_INVALID:
        return "GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        return "PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_AKMP_INVALID:      return "AKMP_INVALID";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        return "UNSUPP_RSN_IE_VERSION";
    case WIFI_REASON_INVALID_RSN_IE_CAP:
        return "INVALID_RSN_IE_CAP";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "802_1X_AUTH_FAILED";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
        return "CIPHER_SUITE_REJECTED";
    case WIFI_REASON_BEACON_TIMEOUT:    return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:       return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:         return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:   return "CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET:      return "AP_TSF_RESET";
    case WIFI_REASON_ROAMING:           return "ROAMING";
    default:                            return "UNKNOWN";
    }
}

static void log_heap_diag(const char *where)
{
    ESP_LOGI(TAG,
             "%s: free_heap=%lu min_free_heap=%lu free_internal=%lu min_internal=%lu",
             where,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

static const char *boot_phase_name(uint32_t phase)
{
    switch (phase) {
    case BOOT_PHASE_START:           return "START";
    case BOOT_PHASE_NVS_READY:       return "NVS_READY";
    case BOOT_PHASE_WIFI_INIT:       return "WIFI_INIT";
    case BOOT_PHASE_WIFI_SCAN:       return "WIFI_SCAN";
    case BOOT_PHASE_WIFI_CONNECTING: return "WIFI_CONNECTING";
    case BOOT_PHASE_WIFI_CONNECTED:  return "WIFI_CONNECTED";
    case BOOT_PHASE_WIFI_FAILED:     return "WIFI_FAILED";
    case BOOT_PHASE_DISPLAY_INIT:    return "DISPLAY_INIT";
    case BOOT_PHASE_DB_OPENING:      return "DB_OPENING";
    case BOOT_PHASE_READY:           return "READY";
    default:                         return "UNKNOWN";
    }
}

static void diag_set_u32(const char *key, uint32_t value)
{
    if (s_diag_nvs != 0) {
        esp_err_t err = nvs_set_u32(s_diag_nvs, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(s_diag_nvs);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "NVS diag write %s failed: %s", key, esp_err_to_name(err));
        }
    }
}

static void diag_set_i32(const char *key, int32_t value)
{
    if (s_diag_nvs != 0) {
        esp_err_t err = nvs_set_i32(s_diag_nvs, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(s_diag_nvs);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "NVS diag write %s failed: %s", key, esp_err_to_name(err));
        }
    }
}

static void diag_set_str(const char *key, const char *value)
{
    if (s_diag_nvs != 0) {
        esp_err_t err = nvs_set_str(s_diag_nvs, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(s_diag_nvs);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "NVS diag write %s failed: %s", key, esp_err_to_name(err));
        }
    }
}

static void diag_note_phase(boot_phase_t phase)
{
    ESP_LOGI(TAG, "Boot phase: %s", boot_phase_name(phase));
    diag_set_u32("last_phase", (uint32_t)phase);
    diag_set_u32("last_us", (uint32_t)(esp_timer_get_time() / 1000ULL));
}

static void diag_init(esp_reset_reason_t reset_reason)
{
    esp_err_t err = nvs_open("bootdiag", NVS_READWRITE, &s_diag_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS bootdiag open failed: %s", esp_err_to_name(err));
        return;
    }

    uint32_t prev_boot_count = 0;
    uint32_t prev_reset = 0;
    uint32_t prev_phase = 0;
    uint32_t prev_ms = 0;
    int32_t prev_disc = 0;
    int32_t prev_rssi = -127;
    uint32_t prev_scan = 0;
    char prev_ip[32] = "unset";
    size_t prev_ip_len = sizeof(prev_ip);

    (void)nvs_get_u32(s_diag_nvs, "boot_count", &prev_boot_count);
    (void)nvs_get_u32(s_diag_nvs, "last_reset", &prev_reset);
    (void)nvs_get_u32(s_diag_nvs, "last_phase", &prev_phase);
    (void)nvs_get_u32(s_diag_nvs, "last_us", &prev_ms);
    (void)nvs_get_i32(s_diag_nvs, "last_disc", &prev_disc);
    (void)nvs_get_i32(s_diag_nvs, "last_rssi", &prev_rssi);
    (void)nvs_get_u32(s_diag_nvs, "last_scan", &prev_scan);
    (void)nvs_get_str(s_diag_nvs, "last_ip", prev_ip, &prev_ip_len);

    ESP_LOGI(TAG,
             "Previous boot diag: boot=%lu reset=%s (%lu) phase=%s at=%lums "
             "scan=%lu rssi=%ld disc=%ld (%s) ip=%s",
             (unsigned long)prev_boot_count,
             reset_reason_name((esp_reset_reason_t)prev_reset),
             (unsigned long)prev_reset,
             boot_phase_name(prev_phase),
             (unsigned long)prev_ms,
             (unsigned long)prev_scan,
             (long)prev_rssi,
             (long)prev_disc,
             wifi_reason_name(prev_disc),
             prev_ip);

    s_boot_count = prev_boot_count + 1;
    diag_set_u32("boot_count", s_boot_count);
    diag_set_u32("last_reset", (uint32_t)reset_reason);
    diag_set_u32("last_phase", BOOT_PHASE_START);
    diag_set_u32("last_us", (uint32_t)(esp_timer_get_time() / 1000ULL));
    diag_set_i32("last_disc", 0);
    diag_set_i32("last_rssi", -127);
    diag_set_u32("last_scan", 0);
    diag_set_str("last_ip", "0.0.0.0");
}

static void wifi_log_scan_results(void)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = (uint8_t *)CONFIG_WORD2VEC_WIFI_SSID,
        .show_hidden = true,
    };

    ESP_LOGI(TAG, "Scanning for SSID '%s' before connect", CONFIG_WORD2VEC_WIFI_SSID);
    diag_note_phase(BOOT_PHASE_WIFI_SCAN);
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed before connect: %s", esp_err_to_name(err));
        return;
    }

    uint16_t ap_count = WIFI_SCAN_MAX_APS;
    wifi_ap_record_t aps[WIFI_SCAN_MAX_APS] = { 0 };
    err = esp_wifi_scan_get_ap_records(&ap_count, aps);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan results failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Wi-Fi scan returned %u AP record(s)", ap_count);
    s_wifi_last_scan_count = ap_count;
    s_wifi_last_scan_rssi = (ap_count > 0) ? aps[0].rssi : -127;
    diag_set_u32("last_scan", ap_count);
    diag_set_i32("last_rssi", s_wifi_last_scan_rssi);
    for (uint16_t i = 0; i < ap_count; i++) {
        ESP_LOGI(TAG,
                 "SSID match %u: bssid=" MACSTR " channel=%u rssi=%d authmode=%u",
                 i + 1,
                 MAC2STR(aps[i].bssid),
                 aps[i].primary,
                 aps[i].rssi,
                 aps[i].authmode);
    }
}

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
        diag_set_i32("last_disc", disc->reason);
        if (s_wifi_retries < WIFI_MAX_RETRIES) {
            s_wifi_retries++;
            ESP_LOGW(TAG, "Wi-Fi disconnected: retry %d/%d, reason=%d (%s)",
                     s_wifi_retries, WIFI_MAX_RETRIES, disc->reason,
                     wifi_reason_name(disc->reason));
            log_heap_diag("Wi-Fi disconnect");
            xEventGroupSetBits(s_wifi_events, WIFI_RETRY_BIT);
        } else {
            ESP_LOGE(TAG, "Wi-Fi failed after %d retries, last reason=%d (%s)",
                     s_wifi_retries, disc->reason, wifi_reason_name(disc->reason));
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started");
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *conn = (wifi_event_sta_connected_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi associated: channel=%u, authmode=%u, aid=%u",
                 conn->channel, conn->authmode, conn->aid);
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        diag_set_str("last_ip", s_ip_str);
        diag_note_phase(BOOT_PHASE_WIFI_CONNECTED);
        s_wifi_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(void) {
    diag_note_phase(BOOT_PHASE_WIFI_INIT);
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return false;
    }

    log_heap_diag("Before Wi-Fi init");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    log_heap_diag("After esp_wifi_init");
    /* Use RAM storage only — prevents stale NVS config from overriding our
       settings on power cycle (works-after-flash / fails-after-replug issue) */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "Wi-Fi storage=RAM, mode=STA, power-save=NONE");

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
    ESP_LOGI(TAG, "Starting Wi-Fi now, uptime=%lld us", esp_timer_get_time());
    ESP_ERROR_CHECK(esp_wifi_start());
    log_heap_diag("After esp_wifi_start");
    wifi_log_scan_results();
    diag_note_phase(BOOT_PHASE_WIFI_CONNECTING);
    ESP_ERROR_CHECK(esp_wifi_connect());

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS);
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
            diag_note_phase(BOOT_PHASE_WIFI_FAILED);
            return false;
        }
        if (bits & WIFI_RETRY_BIT) {
            uint32_t delay_ms = WIFI_RETRY_DELAY_MS;
            if (s_wifi_retries > 10) {
                delay_ms = WIFI_RETRY_DELAY_MS * 2;
            }
            if (s_wifi_retries > 20) {
                delay_ms = WIFI_RETRY_DELAY_MS * 4;
            }
            ESP_LOGI(TAG, "Wi-Fi retry backoff %lu ms",
                     (unsigned long)delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            if (wifi_reason_needs_restart(s_wifi_last_disconnect_reason)) {
                ESP_LOGI(TAG, "Restarting Wi-Fi after reason=%d (%s)",
                         s_wifi_last_disconnect_reason,
                         wifi_reason_name(s_wifi_last_disconnect_reason));
                ESP_ERROR_CHECK(esp_wifi_stop());
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
                ESP_ERROR_CHECK(esp_wifi_start());
                ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
                wifi_log_scan_results();
                ESP_ERROR_CHECK(esp_wifi_connect());
            } else {
                ESP_ERROR_CHECK(esp_wifi_connect());
            }
        }
    }
}

/* ── app_main ───────────────────────────────────────────────────────────── */

void app_main(void) {
    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "Boot: reset_reason=%s (%d), uptime=%lld us",
             reset_reason_name(reset_reason), reset_reason, esp_timer_get_time());
    log_heap_diag("Boot");

    /* NVS (required by Wi-Fi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    log_heap_diag("After NVS init");
    diag_init(reset_reason);
    diag_note_phase(BOOT_PHASE_NVS_READY);

    /* Register custom SD storage driver for nn20db (must be before any nn20db call) */
    sd_storage_register_driver();

    /* Display first: low backlight gives immediate user feedback at boot. */
    diag_note_phase(BOOT_PHASE_DISPLAY_INIT);
    display_init();
    display_set_backlight_percent(LCD_BOOT_BACKLIGHT_PERCENT);

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s", CONFIG_WORD2VEC_WIFI_SSID);
    bool wifi_ok = wifi_connect();

    if (wifi_ok) {
        ESP_LOGI(TAG, "Wi-Fi connected, IP: %s", s_ip_str);
        display_set_ip(s_ip_str);
        display_set_backlight_percent(LCD_WIFI_BACKLIGHT_PERCENT);
    } else {
        ESP_LOGW(TAG, "Wi-Fi connection failed — running without network");
        display_set_ip("offline");
    }


    /* open nn20db */
    diag_note_phase(BOOT_PHASE_DB_OPENING);
    ESP_LOGI(TAG, "Opening nn20db at %s ...", DB_PATH);
    NN20DB *db = NULL;
    int rc = nn20db_open_with_config(&s_nn20db_config, &db);
    if (rc != NN20DB_ERROR_OK || db == NULL) {
        ESP_LOGE(TAG, "Failed to open DB (rc=%d)", rc);
        /* display error and halt */
        display_show_idle();
        display_set_ip("DB open failed!");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "nn20db opened OK");

    /* Log SD card contents for diagnostics */
#ifdef CONFIG_WORD2VEC_LOG_SD_CONTENTS
    sd_storage_log_contents("/sdcard");
#else
    ESP_LOGI(TAG, "SD content listing disabled (enable CONFIG_WORD2VEC_LOG_SD_CONTENTS for verbose dump)");
#endif

    /* start TCP search server */
    net_server_start(db);
    ESP_LOGI(TAG, "TCP server started on port %d", NET_SERVER_PORT);
    display_show_idle();

    /* idle — the server task drives further activity */
    ESP_LOGI(TAG, "Ready. Waiting for queries on %s:%d",
             s_ip_str, NET_SERVER_PORT);
    diag_note_phase(BOOT_PHASE_READY);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

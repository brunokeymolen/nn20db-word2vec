/*
 * display.c — LVGL UI for Waveshare ESP32-S3-Touch-LCD-1.47
 *
 * Hardware:
 *   Display:  JD9853, SPI (172 × 320 IPS, portrait)
 *   Touch:    AXS5106L, I2C
 *   Board:    Waveshare ESP32-S3-Touch-LCD-1.47
 *
 * Pin assignments (from Waveshare official ESP-IDF factory demo / schematic):
 *   SPI LCD:
 *     SCK  = GPIO 38
 *     MOSI = GPIO 39
 *     CS   = GPIO 21
 *     DC   = GPIO 45
 *     RST  = GPIO 40
 *     BL   = GPIO 46
 *   I2C touch:
 *     SDA  = GPIO 42
 *     SCL  = GPIO 41
 *     INT  = GPIO 47 (optional)
 *     RST  = GPIO 48 (optional)
 *
 * The implementation uses:
 *   - esp_lcd panel API (IDF component) for low-level SPI LCD driving
 *   - LVGL v8 for the UI (idf_component_manager: lvgl/lvgl + esp_lvgl_port)
 *
 * NOTE: Pin assignments and init sequences should be verified against the
 * exact Waveshare board revision you have. The numbers above are taken from
 * the Waveshare wiki and example code at time of writing.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "display.h"

static const char *TAG = "display";

/* ── pin definitions ─────────────────────────────────────────────────────── */

#define LCD_SPI_HOST     SPI2_HOST
#define LCD_WIDTH        320   /* landscape: physical 320 px wide */
#define LCD_HEIGHT       172   /* landscape: physical 172 px tall */
#define LCD_BIT_DEPTH    16

#define PIN_LCD_SCK      38
#define PIN_LCD_MOSI     39
#define PIN_LCD_CS       21
#define PIN_LCD_DC       45
#define PIN_LCD_RST      40
#define PIN_LCD_BL       46

#define PIN_TOUCH_SDA    42
#define PIN_TOUCH_SCL    41
/* INT and RST are optional; set to -1 to skip */
#define PIN_TOUCH_INT    47
#define PIN_TOUCH_RST    48

#define TOUCH_I2C_PORT    I2C_NUM_0
#define TOUCH_ADDR        0x3B  /* AXS5106L default address */

/* ── LVGL objects (global within this TU) ────────────────────────────────── */

static SemaphoreHandle_t s_lvgl_mux;
static lv_disp_t        *s_disp;

/* Screen containers */
static lv_obj_t *s_scr_idle;
static lv_obj_t *s_scr_searching;
static lv_obj_t *s_scr_results;

/* Idle screen widgets */
static lv_obj_t *s_lbl_title;      /* "nn20db" */
static lv_obj_t *s_lbl_subtitle;   /* "word2vec" */
static lv_obj_t *s_lbl_ip;
static lv_obj_t *s_lbl_waiting;

/* Searching screen widgets */
static lv_obj_t *s_spinner;
static lv_obj_t *s_lbl_searching;

/* Results screen widgets */
static lv_obj_t *s_list_results;
static lv_obj_t *s_lbl_result_title;

/* ── JD9853 init sequence ────────────────────────────────────────────────── */
/*
 * The init sequence below is derived from the Waveshare Arduino GFX example
 * for this board. Adjust if your board revision uses different settings.
 * Each entry: { cmd, data_len, data... }
 */
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_len;
} lcd_init_cmd_t;

static const lcd_init_cmd_t JD9853_INIT_CMDS[] = {
    { 0xDF, { 0x98, 0x51, 0xE9 }, 3 },
    { 0xB7, { 0x00 }, 1 },
    { 0xC0, { 0x10, 0x0E }, 2 },
    { 0xC1, { 0x10 }, 1 },
    { 0xC2, { 0x01 }, 1 },
    { 0xC5, { 0x0F }, 1 },
    { 0xB1, { 0xA0 }, 1 },
    { 0xB4, { 0x02 }, 1 },
    { 0x36, { 0x20 }, 1 },   /* MADCTL — landscape (MV only): swap axes, no mirror, RGB order */
    { 0x3A, { 0x55 }, 1 },   /* COLMOD — 16 bpp */
    { 0xE0, { 0x00,0x01,0x08,0x09,0x0C,0x29,0x2F,
              0x43,0x50,0x3A,0x15,0x16,0x21,0x22,0x0F }, 15 },
    { 0xE1, { 0x00,0x11,0x15,0x0C,0x09,0x06,0x2C,
              0x43,0x50,0x3A,0x14,0x15,0x21,0x22,0x0F }, 15 },
    { 0x11, {}, 0 },  /* Sleep out */
    { 0x29, {}, 0 },  /* Display on */
};

static void jd9853_init(esp_lcd_panel_io_handle_t io) {
    for (size_t i = 0; i < sizeof(JD9853_INIT_CMDS)/sizeof(JD9853_INIT_CMDS[0]); i++) {
        const lcd_init_cmd_t *c = &JD9853_INIT_CMDS[i];
        esp_lcd_panel_io_tx_param(io, c->cmd, c->data, c->data_len);
        if (c->cmd == 0x11) vTaskDelay(pdMS_TO_TICKS(120));
        if (c->cmd == 0x29) vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ── LVGL flush callback ──────────────────────────────────────────────────── */

static esp_lcd_panel_handle_t s_panel = NULL;

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_map)
{
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                              color_map);
    lv_disp_flush_ready(drv);
}

/* ── LVGL tick timer ─────────────────────────────────────────────────────── */

static void lvgl_tick_cb(void *arg) {
    (void)arg;
    lv_tick_inc(2);
}

/* ── LVGL handler task ───────────────────────────────────────────────────── */

static void lvgl_task(void *arg) {
    (void)arg;
    while (1) {
        if (xSemaphoreTake(s_lvgl_mux, portMAX_DELAY)) {
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ── screen builders ─────────────────────────────────────────────────────── */

static void build_idle_screen(void) {
    s_scr_idle = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_idle, lv_color_hex(0x0D1117), 0);

    /* line 1: "nn20db" */
    s_lbl_title = lv_label_create(s_scr_idle);
    lv_label_set_text(s_lbl_title, "nn20db");
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_36, 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, 6);

    /* line 2: "word2vec" */
    s_lbl_subtitle = lv_label_create(s_scr_idle);
    lv_label_set_text(s_lbl_subtitle, "word2vec");
    lv_obj_set_style_text_color(s_lbl_subtitle, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_style_text_font(s_lbl_subtitle, &lv_font_montserrat_36, 0);
    lv_obj_align(s_lbl_subtitle, LV_ALIGN_TOP_MID, 0, 48);

    /* horizontal rule */
    lv_obj_t *line = lv_line_create(s_scr_idle);
    static lv_point_t pts[2] = { {0, 0}, {LCD_WIDTH, 0} };
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_line_width(line, 1, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 92);

    /* IP address */
    s_lbl_ip = lv_label_create(s_scr_idle);
    lv_label_set_text(s_lbl_ip, "Wi-Fi: connecting...");
    lv_obj_set_style_text_color(s_lbl_ip, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_style_text_font(s_lbl_ip, &lv_font_montserrat_20, 0);
    lv_obj_align(s_lbl_ip, LV_ALIGN_TOP_MID, 0, 100);

    /* waiting label */
    s_lbl_waiting = lv_label_create(s_scr_idle);
    lv_label_set_text(s_lbl_waiting, "Waiting for query...");
    lv_obj_set_style_text_color(s_lbl_waiting, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_style_text_font(s_lbl_waiting, &lv_font_montserrat_20, 0);
    lv_obj_align(s_lbl_waiting, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void build_searching_screen(void) {
    s_scr_searching = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_searching, lv_color_hex(0x0D1117), 0);

    /* spinner left, label right — uses the wide landscape space */
    s_spinner = lv_spinner_create(s_scr_searching, 1000, 60);
    lv_obj_set_size(s_spinner, 80, 80);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x58A6FF), LV_PART_INDICATOR);
    lv_obj_align(s_spinner, LV_ALIGN_CENTER, -60, 0);

    s_lbl_searching = lv_label_create(s_scr_searching);
    lv_label_set_text(s_lbl_searching, "Searching...");
    lv_obj_set_style_text_color(s_lbl_searching, lv_color_hex(0x8B949E), 0);
    lv_obj_set_style_text_font(s_lbl_searching, &lv_font_montserrat_24, 0);
    lv_obj_align(s_lbl_searching, LV_ALIGN_CENTER, 40, 0);
}

static void build_results_screen(void) {
    s_scr_results = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_results, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_pad_all(s_scr_results, 4, 0);

    s_lbl_result_title = lv_label_create(s_scr_results);
    lv_label_set_text(s_lbl_result_title, "Results");
    lv_obj_set_style_text_color(s_lbl_result_title, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_style_text_font(s_lbl_result_title, &lv_font_montserrat_20, 0);
    lv_obj_align(s_lbl_result_title, LV_ALIGN_TOP_MID, 0, 4);

    s_list_results = lv_list_create(s_scr_results);
    lv_obj_set_size(s_list_results, LCD_WIDTH - 8, LCD_HEIGHT - 34);
    lv_obj_align(s_list_results, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(s_list_results, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_color(s_list_results, lv_color_hex(0x30363D), 0);
}

/* ── public API ──────────────────────────────────────────────────────────── */

void display_init(void) {
    ESP_LOGI(TAG, "Initialising display (JD9853, 320x172 landscape)");

    /* backlight */
    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(PIN_LCD_BL, 0); /* off during init */

    /* SPI bus */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = PIN_LCD_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = PIN_LCD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* LCD panel IO (SPI) */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_LCD_DC,
        .cs_gpio_num       = PIN_LCD_CS,
        .pclk_hz           = 40 * 1000 * 1000,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                              &io_cfg, &io_handle));

    /* Reset */
    if (PIN_LCD_RST >= 0) {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = (1ULL << PIN_LCD_RST),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(PIN_LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(PIN_LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    /* Custom init sequence for JD9853 */
    jd9853_init(io_handle);

    /* Create a minimal panel handle for draw_bitmap */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .color_space    = ESP_LCD_COLOR_SPACE_RGB, /* MADCTL BGR=0, so RGB order */
        .bits_per_pixel = LCD_BIT_DEPTH,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel));
    /*
     * JD9853 controller has a 240-row address space in landscape (was 240 cols
     * in portrait).  The 172 physical rows are centered, so row gap = 34.
     * Column gap is 0 because the controller column count matches 320.
     */
    esp_lcd_panel_set_gap(s_panel, 0, 34);
    /* do NOT call swap_xy / mirror here — they re-issue MADCTL and would
       override the value we already sent in jd9853_init() */

    /* LVGL init */
    s_lvgl_mux = xSemaphoreCreateMutex();
    lv_init();

    static lv_color_t buf1[LCD_WIDTH * 40];
    static lv_color_t buf2[LCD_WIDTH * 40];
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_WIDTH * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res    = LCD_WIDTH;
    disp_drv.ver_res    = LCD_HEIGHT;
    disp_drv.flush_cb   = lvgl_flush_cb;
    disp_drv.draw_buf   = &draw_buf;
    s_disp = lv_disp_drv_register(&disp_drv);

    /* LVGL tick timer (2 ms) */
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2 * 1000));

    /* build screens */
    build_idle_screen();
    build_searching_screen();
    build_results_screen();

    /* show idle screen */
    lv_disp_load_scr(s_scr_idle);

    /* LVGL handler task */
    xTaskCreate(lvgl_task, "lvgl", 8 * 1024, NULL, tskIDLE_PRIORITY + 3, NULL);
    gpio_set_level(PIN_LCD_BL, 1);

    ESP_LOGI(TAG, "Display init complete");
}

void display_set_ip(const char *ip) {
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(100))) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Wi-Fi: %s", ip);
        lv_label_set_text(s_lbl_ip, buf);
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_show_searching(void) {
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(100))) {
        lv_disp_load_scr(s_scr_searching);
        xSemaphoreGive(s_lvgl_mux);
    }
}

void display_show_results(const char words[][32], int count, float ms) {
    if (!xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(200))) return;

    /* clear previous list */
    lv_obj_clean(s_list_results);

    /* update title */
    char title[64];
    snprintf(title, sizeof(title), "Top %d  (%.1f ms)", count, (double)ms);
    lv_label_set_text(s_lbl_result_title, title);

    /* add each result as a list button */
    for (int i = 0; i < count; i++) {
        char label[48];   /* 11 (int) + 2 (". ") + 32 (word) + NUL */
        snprintf(label, sizeof(label), "%d. %.32s", i + 1, words[i]);

        lv_obj_t *btn = lv_list_add_btn(s_list_results, NULL, label);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xC9D1D9), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x161B22), 0);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_20, 0);
    }

    lv_disp_load_scr(s_scr_results);
    xSemaphoreGive(s_lvgl_mux);
}

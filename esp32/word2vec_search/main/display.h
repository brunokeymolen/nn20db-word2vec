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

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * display.h — LVGL UI for the Waveshare ESP32-S3-Touch-LCD-1.47
 *
 * Display:  172 × 320  IPS, JD9853 (SPI), AXS5106L touch (I2C), portrait.
 *
 * Three UI states:
 *   idle      — title + WiFi IP + "Waiting for query..."
 *   searching — animated spinner
 *   results   — scrollable list of top-k results
 */

/* Initialise LVGL, the SPI LCD driver and the I2C touch driver. */
void display_init(void);

/* Set LCD backlight brightness, 0..100 percent. */
void display_set_backlight_percent(uint8_t percent);

/* Transition to the idle screen. */
void display_show_idle(void);

/* Update the idle screen with the device's Wi-Fi IP address. */
void display_set_ip(const char *ip);

/* Set the database name shown next to "word2vec" / "Results" (8.3 basename). */
void display_set_db_name(const char *name);

/* Transition to the searching state (spinner). */
void display_show_searching(void);

/*
 * Show the results list.
 *   words   — array of null-terminated word strings (each ≤ 32 bytes)
 *   count   — number of entries
 *   ef      — search ef value used for the query
 *   ms      — search duration in milliseconds
 */
void display_show_results(const char words[][32], int count, int ef, float ms);

#ifdef __cplusplus
}
#endif

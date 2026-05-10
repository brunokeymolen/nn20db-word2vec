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

/* Update the idle screen with the device's Wi-Fi IP address. */
void display_set_ip(const char *ip);

/* Transition to the searching state (spinner). */
void display_show_searching(void);

/*
 * Show the results list.
 *   words   — array of null-terminated word strings (each ≤ 32 bytes)
 *   count   — number of entries
 *   ms      — search duration in milliseconds
 */
void display_show_results(const char words[][32], int count, float ms);

#ifdef __cplusplus
}
#endif

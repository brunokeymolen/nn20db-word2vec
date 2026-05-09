idf_component_register(
    SRCS "main.c" "net_server.c" "display.c" "sd_storage.c"
    INCLUDE_DIRS "."
    REQUIRES nn20db esp_wifi esp_netif esp_event nvs_flash fatfs sdmmc esp_lcd lvgl esp_lvgl_port
)

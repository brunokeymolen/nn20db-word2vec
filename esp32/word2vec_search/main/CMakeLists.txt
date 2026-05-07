idf_component_register(
    SRCS "main.c" "net_server.c" "display.c"
    INCLUDE_DIRS "."
    REQUIRES nn20db esp_wifi esp_netif esp_event nvs_flash fatfs sdmmc esp_lcd
)

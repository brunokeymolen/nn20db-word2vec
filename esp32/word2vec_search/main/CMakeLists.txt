idf_component_register(
    SRCS "main.c" "net_server.c" "display.c" "sd_storage.c"
    INCLUDE_DIRS "."
    REQUIRES nn20db esp_wifi esp_netif esp_event nvs_flash fatfs sdmmc esp_lcd lvgl__lvgl espressif__esp_lvgl_port
)

# Product quantization config toggle
# Override: idf.py -DUSE_PQ=0 build  (or: make build USE_PQ=0)
option(USE_PQ "Use product quantization config" ON)
target_compile_definitions(${COMPONENT_LIB} PRIVATE USE_PQ=$<BOOL:${USE_PQ}>)

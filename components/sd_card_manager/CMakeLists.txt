idf_component_register(
    SRCS "sd_card_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_gpio esp_driver_sdmmc esp_http_server espressif__cjson web_interface
    EMBED_FILES "assets/sd_card.html" "assets/sd_card.js"
)

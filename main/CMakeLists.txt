idf_component_register(SRCS "main.c" "gss_data.c" "gss_preferences.c" "gss_wifi_storage.c" "gss_wifi_radio.c" "gss_system.c" "../shared/gss_wifi_profile.c"
    "../simulator/ui.c" "../simulator/theme.c" "../simulator/game_mode.c" "../simulator/clash_client.c"
    "../simulator/font_16.c" "../simulator/font_20.c" "../simulator/font_countdown.c"
    "../shared/gso_model.c" "../shared/gso_snapshot.c" "../shared/gso_brightness.c" "../shared/gso_link.c" "../shared/gso_home_delivery.c" "../shared/gso_level.c"
    INCLUDE_DIRS "." "../shared" "../simulator"
    REQUIRES gso_lvgl esp_lcd_axs15260 esp_lcd esp_driver_gpio esp_driver_ledc esp_driver_ppa esp_psram esp_timer esp_driver_i2c esp_driver_tsens spi_flash app_update bootloader_support nvs_flash gso_json esp_hw_support esp_wifi esp_wifi_remote esp_hosted esp_netif esp_event lwip esp_http_client)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_CONF_INCLUDE_SIMPLE CJSON_NESTING_LIMIT=32)

# Optimize the renderer and shared model for the actual P4 rather than debug stepping.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

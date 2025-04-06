idf_component_register(SRCS "coredump_manager.c" "led_strip_encoder.c" "OTA.c" "http_server.c" "dns_server.c" "main.c" "LED_button.c" "audio_player.c" "tag_reader.c" "audio_record.c" "http_server.c" "websocket_manager.c" "audio_stream.c" "wifi.c"
                       PRIV_REQUIRES spi_flash espcoredump app_update esp_bootloader_format esp_https_ota wifi_provisioning esp_adc driver esp32-wifi-manager esp_wifi esp_netif esp_event nvs_flash esp_websocket_client fatfs sdmmc esp_driver_rmt
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES server_cert.pem)

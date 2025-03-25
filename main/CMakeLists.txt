idf_component_register(SRCS "main.c" "led_strip_encoder.c" "LED_button.c" "sd_card_utils.c" "audio_player.c" "tag_reader.c" "audio_record.c" "websocket_manager.c" "audio_stream.c" "wifi_manager.c"
                       PRIV_REQUIRES esp_adc driver esp32-wifi-manager esp_wifi esp_netif esp_event nvs_flash esp_websocket_client fatfs sdmmc esp_driver_rmt
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES server_cert.pem)

idf_component_register(SRCS "main.c" "sd_card_utils.c" "audio_player.c" "audio_recorder.c"
                       PRIV_REQUIRES driver esp32-wifi-manager esp_wifi esp_netif esp_event nvs_flash esp_websocket_client fatfs sdmmc
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES server_cert.pem)

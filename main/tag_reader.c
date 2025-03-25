#include <string.h>
#include <stdio.h>
#include "tag_reader.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "rc522.h"
#include "driver/rc522_spi.h"
#include "rc522_picc.h"

#include "websocket_manager.h" // so we can call websocket_manager_send_bin()

static const char *TAG = "RC522_TAG_READER";

// Adjust these per your hardware setup
#define RC522_SPI_BUS_GPIO_MISO    (34)
#define RC522_SPI_BUS_GPIO_MOSI    (23)
#define RC522_SPI_BUS_GPIO_SCLK    (32)
#define RC522_SPI_SCANNER_GPIO_SDA (33)
#define RC522_SCANNER_GPIO_RST     (-1) // soft-reset
#ifndef RC522_PICC_MAX_UID_SIZE
#define RC522_PICC_MAX_UID_SIZE 10
#endif

// Define the external variables
bool s_card_active = false;
uint8_t s_last_uid[RC522_PICC_MAX_UID_SIZE];
size_t s_last_uid_len = 0;

static rc522_spi_config_t driver_config = {
    .host_id = SPI3_HOST,
    .bus_config = &(spi_bus_config_t){
        .miso_io_num = RC522_SPI_BUS_GPIO_MISO,
        .mosi_io_num = RC522_SPI_BUS_GPIO_MOSI,
        .sclk_io_num = RC522_SPI_BUS_GPIO_SCLK,
    },
    .dev_config = {
        .spics_io_num = RC522_SPI_SCANNER_GPIO_SDA,
    },
    .rst_io_num = RC522_SCANNER_GPIO_RST,
};

static rc522_driver_handle_t driver;
static rc522_handle_t scanner;

// Structure we'll pass to the queue
typedef struct {
    bool tag_removed;
    // For new card: store the UID bytes and length
    uint8_t uid[RC522_PICC_MAX_UID_SIZE];
    size_t uid_len;
} rfid_event_t;

// Queue handle
static QueueHandle_t s_rfid_queue = NULL;

// This callback is invoked automatically whenever a tag is detected or removed
static void on_picc_state_changed(void *arg,
                                  esp_event_base_t base,
                                  int32_t event_id,
                                  void *data)
{
    rc522_picc_state_changed_event_t *evt = (rc522_picc_state_changed_event_t *)data;
    rc522_picc_t *picc = evt->picc;

    rfid_event_t msg = {0};

    if (picc->state == RC522_PICC_STATE_ACTIVE) {
        ESP_LOGI(TAG, "New card present");

        s_card_active = true;
        s_last_uid_len = picc->uid.length;
        memcpy(s_last_uid, picc->uid.value, s_last_uid_len);

        msg.tag_removed = false;
        msg.uid_len = picc->uid.length;
        memcpy(msg.uid, picc->uid.value, msg.uid_len);

        xQueueSend(s_rfid_queue, &msg, 0);
    }
    else if (picc->state == RC522_PICC_STATE_IDLE &&
             evt->old_state >= RC522_PICC_STATE_ACTIVE) {
        // Card was removed
        ESP_LOGI(TAG, "Card removed");

        s_card_active = false;
        s_last_uid_len = 0; 
        memset(s_last_uid, 0, sizeof(s_last_uid));

        msg.tag_removed = true;
        // Optional: if you want to know which card was removed, you could 
        // store the old UID before going idle. But typically once removed,
        // we only know "some card" is gone, so we only set tag_removed.
        xQueueSend(s_rfid_queue, &msg, 0);
    }
}

// Task that waits for events from the RFID queue and sends them via WebSocket
static void rfid_task(void *arg)
{
    rfid_event_t evt;
    while (true) {
        if (xQueueReceive(s_rfid_queue, &evt, portMAX_DELAY)) {
            websocket_manager_send_rfid_event(evt.uid, evt.uid_len, evt.tag_removed);
        }
    }
    vTaskDelete(NULL);
}

void tag_reader_init(void)
{
    // Create the RFID event queue
    s_rfid_queue = xQueueCreate(5, sizeof(rfid_event_t));
    if (!s_rfid_queue) {
        ESP_LOGE(TAG, "Failed to create RFID queue");
        return;
    }

    // Initialize RC522 (SPI)
    rc522_spi_create(&driver_config, &driver);
    rc522_driver_install(driver);

    // Create the scanner
    rc522_config_t scanner_config = {
        .driver = driver,
    };
    rc522_create(&scanner_config, &scanner);

    // Register the event callback
    rc522_register_events(scanner, RC522_EVENT_PICC_STATE_CHANGED,
                          on_picc_state_changed, NULL);

    // Start scanning
    rc522_start(scanner);

    // Create the task that will forward RFID events to WebSocket
    xTaskCreatePinnedToCore(
        rfid_task,
        "rfid_task",
        4096,
        NULL,
        5,
        NULL,
        tskNO_AFFINITY
    );

    ESP_LOGI(TAG, "RC522 tag reader initialized");
}

#include "coredump_manager.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "esp_partition.h"  // <--- use partition APIs
#include <string.h>
#include <stdlib.h>

static const char* TAG = "CORE_DUMP_MGR";

static char s_coredump_base64[4096] = {0};
static bool s_coredump_found        = false;

void coredump_manager_check_and_load(void)
{
    size_t coredump_flash_offset = 0;
    size_t coredump_size         = 0;

    // 1) Get offset + size of any coredump
    esp_err_t err = esp_core_dump_image_get(&coredump_flash_offset, &coredump_size);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No coredump or error reading offset: %s", esp_err_to_name(err));
        return;
    }
    if (coredump_size == 0) {
        ESP_LOGI(TAG, "No coredump found (size=0).");
        return;
    }
    ESP_LOGW(TAG, "Coredump found at offset=0x%08X size=%u bytes",
             (unsigned)coredump_flash_offset, (unsigned)coredump_size);

    // 2) Find the actual coredump partition (type=DATA, subtype=COREDUMP)
    const esp_partition_t *coredump_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!coredump_part) {
        ESP_LOGE(TAG, "No coredump partition found in partition table!");
        return;
    }

    // 3) Read coredump bytes into a buffer
    uint8_t *coredump_data = (uint8_t *) malloc(coredump_size);
    if (!coredump_data) {
        ESP_LOGE(TAG, "Failed to malloc %u bytes", (unsigned)coredump_size);
        return;
    }
    err = esp_partition_read(coredump_part,
                             coredump_flash_offset,
                             coredump_data,
                             coredump_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read coredump from partition: %s", esp_err_to_name(err));
        free(coredump_data);
        return;
    }

    // 4) Base64-encode for sending
    memset(s_coredump_base64, 0, sizeof(s_coredump_base64));
    size_t required_base64_len = 4 * ((coredump_size + 2) / 3) + 1;
    if (required_base64_len > sizeof(s_coredump_base64)) {
        ESP_LOGE(TAG, "Coredump is too large to fit in base64 buffer! (need %u, have %u)",
                 (unsigned)required_base64_len, (unsigned)sizeof(s_coredump_base64));
        free(coredump_data);
        return;
    }

    size_t encoded_len = 0;
    int ret = mbedtls_base64_encode((unsigned char*)s_coredump_base64,
                                    sizeof(s_coredump_base64),
                                    &encoded_len,
                                    coredump_data,
                                    coredump_size);
    free(coredump_data);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed base64-encoding coredump (mbedtls ret=%d)", ret);
        return;
    }

    s_coredump_found = true;
    ESP_LOGI(TAG, "Coredump base64 length=%u", (unsigned)encoded_len);

    // 5) Optionally erase coredump so it’s not re-sent next time
    err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase coredump: %s", esp_err_to_name(err));
    }
}

bool coredump_manager_found(void)
{
    return s_coredump_found;
}

const char* coredump_manager_get_base64(void)
{
    return s_coredump_found ? s_coredump_base64 : NULL;
}

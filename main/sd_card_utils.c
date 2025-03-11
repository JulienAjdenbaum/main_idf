#include <stdio.h>
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <dirent.h>
#include "sd_card_utils.h"

const char mount_point[] = "/sdcard";
static sdmmc_card_t* card;

esp_err_t initialize_sd_card(void)
{
    // Use SPI peripheral
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = HSPI_HOST; // Use HSPI_HOST for SPI

    // Configure the SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = 23,
        .miso_io_num = 19,
        .sclk_io_num = 18,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        printf("Failed to initialize bus.\n");
        return ret;
    }

    // Configure the SD card
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = 5;
    slot_config.host_id = host.slot;

    // Mount the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        printf("Failed to mount filesystem. Error: %s\n", esp_err_to_name(ret));
        spi_bus_free(host.slot);
    } else {
        sdmmc_card_print_info(stdout, card);
    }

    return ret;
}

void list_files_in_sd_card(void)
{
    DIR* dir = opendir(mount_point);
    if (dir == NULL) {
        printf("Failed to open directory\n");
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        printf("Found file: %s\n", entry->d_name);
    }
    closedir(dir);
}

void unmount_sd_card(void)
{
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    printf("Card unmounted\n");
    spi_bus_free(HSPI_HOST);
} 

esp_err_t write_file_to_sd_card(const char* filename, const char* content) {
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/%s", filename);

    FILE* f = fopen(path, "w");
    if (f == NULL) {
        printf("Failed to open file for writing: %s\n", filename);
        return ESP_FAIL;
    }

    fprintf(f, "%s", content);
    fclose(f);
    printf("File written: %s\n", filename);
    return ESP_OK;
}
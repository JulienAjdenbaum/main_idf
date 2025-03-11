#ifndef SD_CARD_UTILS_H
#define SD_CARD_UTILS_H

#include "esp_err.h"

esp_err_t initialize_sd_card(void);
void list_files_in_sd_card(void);
void unmount_sd_card(void);
esp_err_t write_file_to_sd_card(const char* filename, const char* content);

// Add extern declaration
extern const char mount_point[];

#endif // SD_CARD_UTILS_H

#ifndef SD_CARD_UTILS_H
#define SD_CARD_UTILS_H

#include "esp_err.h"

esp_err_t initialize_sd_card(void);
void list_files_in_sd_card(void);
void unmount_sd_card(void);

#endif // SD_CARD_UTILS_H

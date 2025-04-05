#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if there's a coredump in flash. If found, base64-encode it,
 *        store it internally, and optionally erase it from flash.
 */
void coredump_manager_check_and_load(void);

/**
 * @brief Return true if a coredump was found & successfully base64-encoded
 */
bool coredump_manager_found(void);

/**
 * @brief Get the pointer to the base64 string of the coredump (if found).
 *        Returns NULL if none found.
 */
const char* coredump_manager_get_base64(void);

#ifdef __cplusplus
}
#endif

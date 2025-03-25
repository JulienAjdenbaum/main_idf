#ifndef TAG_READER_H
#define TAG_READER_H

#include <stdbool.h> // for bool
#include <stddef.h>  // for size_t
#ifdef __cplusplus
extern "C" {
#endif

extern bool s_card_active;
extern uint8_t s_last_uid[10];
extern size_t s_last_uid_len;

void tag_reader_init(void);

#ifdef __cplusplus
}
#endif

#endif // TAG_READER_H

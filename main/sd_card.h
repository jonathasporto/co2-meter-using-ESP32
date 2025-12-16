#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>

bool init_sd_card(void);

void write_data_to_csv(const char *data);

void close_current_file(void);

#endif // SD_CARD_H

#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>
#include "esp_err.h"  // *** ADICIONAR ESTA LINHA ***

bool init_sd_card(void);
void write_data_to_csv(const char *data); 
static void open_new_csv_file(void);
void close_current_file(void);

#endif // SD_CARD_H

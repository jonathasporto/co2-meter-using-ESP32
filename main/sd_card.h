#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>

bool init_sd_card(void);
<<<<<<< HEAD
void write_data_to_csv(const char *data);
=======
void write_data_to_csv(const char *data); 
// static void open_new_csv_file(void);
>>>>>>> 18ec69660fbe7c446d725b940230018a21639db1
void close_current_file(void);

#endif // SD_CARD_H

#ifndef RTC_H
#define RTC_H

#include <time.h>
#include <stdbool.h>

struct tm; // Forward declaration

void initialize_rtc(void);
void get_current_date_time(char *date_str, char *time_str);
void get_current_date_time_filename(char *date_time_str);
void read_time_from_ds1302(struct tm *timeinfo);

#endif // RTC_H
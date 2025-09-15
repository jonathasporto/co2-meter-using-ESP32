#ifndef RTC_H
#define RTC_H

#include <time.h>

void initialize_rtc(void);
void get_current_date_time(char *date_str, char *time_str);
void get_current_date_time_filename(char *date_time_str);

#endif // RTC_H
#ifndef RTC_H
#define RTC_H

#include <time.h>
#include <stdbool.h>

void initialize_rtc(void);
void get_current_date_time(char *date_str, char *time_str);
void get_current_date_time_filename(char *date_time_str);
void read_time_from_ds1302(struct tm *timeinfo);
bool rtc_set_time(int year, int month, int day, int hour, int minute, int second);  
bool rtc_sync_from_system(void);

#endif // RTC_H
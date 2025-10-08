#ifndef RTC_H
#define RTC_H

#include <time.h>
#include <stdbool.h>
#include <stddef.h>

// Declaração das funções para que outros arquivos possam usá-las.

void initialize_rtc(void);
bool read_time_from_ds1302(struct tm *timeinfo);
void set_time_on_ds1302(const struct tm *timeinfo);
void set_manual_time_rtc(int year, int month, int day, int hour, int minute, int second);
void get_current_date_time(char *date_str, size_t date_len, char *time_str, size_t time_len);
void get_current_date_time_filename(char *date_time_str, size_t len);
void adjust_rtc_offset(int offset_seconds);

#endif // RTC_H
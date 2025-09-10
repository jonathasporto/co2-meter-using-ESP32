#include "rtc.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "RTC";

static EventGroupHandle_t sntp_event_group;
const int TIME_SYNC_BIT = BIT0;

static void time_sync_notification_cb(struct timeval *tv) {
    xEventGroupSetBits(sntp_event_group, TIME_SYNC_BIT);
}

void initialize_rtc(void) {
    sntp_event_group = xEventGroupCreate();

    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    // Aguarda a sincronização do tempo
    EventBits_t bits = xEventGroupWaitBits(sntp_event_group, TIME_SYNC_BIT, pdTRUE, pdTRUE, pdMS_TO_TICKS(10000));

    if (bits & TIME_SYNC_BIT) {
        ESP_LOGI(TAG, "Time synchronized");
    } else {
        ESP_LOGW(TAG, "Failed to synchronize time");
    }
}

void get_current_date_time(char *date_str, char *time_str) {
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(date_str, 11, "%Y-%m-%d", &timeinfo); // YYYY-MM-DD
    strftime(time_str, 9, "%H:%M:%S", &timeinfo);  // HH:MM:SS
}

void get_current_date_time_filename(char *date_time_str) {
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(date_time_str, 20, "%Y-%m-%d_%H-%M-%S", &timeinfo); // YYYY-MM-DD_HH-MM-SS
}

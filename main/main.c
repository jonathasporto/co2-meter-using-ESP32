#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "dht.h"
#include "co2_sensor_task.h"
#include "sd_card.h"
#include "http_server.h"
#include "rtc.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_sleep.h"  

#define BUTTON_PIN GPIO_NUM_14
#define DHT_PIN 4 // Pino do DHT22

//#define MODO_DE_TESTE // Comente esta linha para voltar ao modo normal (horários reais)

static const char *TAG = "CO2-METER-SDCARD";
static bool wifi_active = false;
static httpd_handle_t server_handle = NULL; // Para controlar o servidor HTTP

void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    (void)netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32_CO2",
            .ssid_len = strlen("ESP32_CO2"),
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    if (strlen((const char*)wifi_config.ap.password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialized in AP mode. SSID: %s", wifi_config.ap.ssid);
}

static void button_task(void *arg) {
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);

    while(1) {
        if (gpio_get_level(BUTTON_PIN) == 1 && !wifi_active) { // Botão pressionado (nível alto com pull-up)
            wifi_active = true;
            ESP_LOGI(TAG, "Button pressed! Activating Wi-Fi and HTTP Server for 5 minutes...");

            wifi_init_softap();
            start_http_server(&server_handle);

            vTaskDelay(pdMS_TO_TICKS(300000)); // 5 minutos

            ESP_LOGI(TAG, "5 minutes timeout. Deactivating Wi-Fi...");
            if (server_handle) {
                httpd_stop(server_handle);
                server_handle = NULL;
            }
            esp_wifi_stop();
            esp_wifi_deinit();
            esp_netif_deinit();
            wifi_active = false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void measurement_scheduler_task(void *arg) {
    // Inicializações
    nvs_flash_init();
    initialize_rtc();

    if (!init_sd_card()) {
        ESP_LOGE(TAG, "Failed to initialize SD card at startup. Halting scheduler task.");
        vTaskDelete(NULL); // Encerra a tarefa se o SD card não for inicializado
    }

    co2_sensor_power_control(true); // Liga o sensor de CO2 permanentemente

    while (1) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

#ifdef MODO_DE_TESTE
        bool should_measure = true;
        ESP_LOGI(TAG, "TEST MODE: Forcing measurement.");
#else
        bool is_day_time = (timeinfo.tm_hour > 6 || (timeinfo.tm_hour == 6 && timeinfo.tm_min >= 30)) && (timeinfo.tm_hour < 18 || (timeinfo.tm_hour == 18 && timeinfo.tm_min < 30));
        bool is_night_time = !is_day_time;

        bool is_on_minute_schedule = (timeinfo.tm_min == 0 || timeinfo.tm_min == 30);
        bool is_in_measurement_window = 
            (timeinfo.tm_hour >= 7 && timeinfo.tm_hour < 9) ||
            (timeinfo.tm_hour >= 11 && timeinfo.tm_hour < 13) ||
            (timeinfo.tm_hour >= 16 && timeinfo.tm_hour < 18) ||
            (timeinfo.tm_hour == 9 && timeinfo.tm_min == 0) ||
            (timeinfo.tm_hour == 13 && timeinfo.tm_min == 0) ||
            (timeinfo.tm_hour == 18 && timeinfo.tm_min == 0);
        
        bool should_measure = is_day_time && is_on_minute_schedule && is_in_measurement_window;
#endif

        if (should_measure) {
            ESP_LOGI(TAG, "Scheduled measurement time! Performing data acquisition.");
            perform_single_measurement();
            close_current_file();
        } else {
            ESP_LOGI(TAG, "Not a scheduled measurement time. Checking sleep mode.");
        }

#ifndef MODO_DE_TESTE
        if (is_night_time) {
            ESP_LOGI(TAG, "Night time (18:30-06:30). Entering Light-sleep for 30 minutes.");
            esp_sleep_enable_timer_wakeup(30 * 60 * 1000000ULL);
            esp_light_sleep_start();
        } else { // is_day_time
            ESP_LOGI(TAG, "Day time (06:30-18:30). Waiting for 30 minutes (Modem-sleep).");
            vTaskDelay(pdMS_TO_TICKS(30 * 60 * 1000));
        }
#else
        ESP_LOGI(TAG, "TEST MODE: Waiting for 15 seconds.");
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
#endif
    }
}

void app_main(void) {
    // Inicializa o NVS, necessário para o Wi-Fi e outras configurações
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Garante que o Wi-Fi inicie desligado
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_deinit();
    
    // Cria as tarefas FreeRTOS, fixando-as em núcleos específicos para evitar conflitos
    xTaskCreatePinnedToCore(measurement_scheduler_task, "Scheduler", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(button_task, "Button/Web", 4096, NULL, 10, NULL, 1);
}
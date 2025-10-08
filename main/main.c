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

// #define MODO_DE_TESTE // Comente esta linha para voltar ao modo normal (horários reais)

static const char *TAG = "CO2-METER-SDCARD";

void wifi_init_softap(void) {
    // Inicializa a pilha TCP/IP
    ESP_ERROR_CHECK(esp_netif_init());

    // Cria o loop de eventos padrão
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Inicializa o Wi-Fi com as configurações padrão
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Cria a interface de rede Wi-Fi AP padrão
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    (void)netif;

    // Configura o Wi-Fi em modo AP
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32_CO2",
            .ssid_len = strlen("ESP32_CO2"),
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    if (strlen("12345678") == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // Define o modo Wi-Fi para Access Point
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Define as configurações do Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    // Inicia o Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialized in AP mode. SSID: %s", wifi_config.ap.ssid);
}

static void button_task(void *arg) {
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    bool wifi_active = false;
    
    while(1) {
        if (gpio_get_level(BUTTON_PIN) == 1 && !wifi_active) { 
            wifi_active = true;
            ESP_LOGI(TAG, "Button pressed! Activating Wi-Fi and HTTP Server for 5 minutes...");
            
            // Inicializa a pilha TCP/IP e Wi-Fi em modo AP
            wifi_init_softap(); 
            // Inicia o servidor HTTP
            start_http_server();

            vTaskDelay(pdMS_TO_TICKS(300000)); // 5 minutos

            ESP_LOGI(TAG, "5 minutes timeout. Deactivating Wi-Fi...");
            esp_wifi_stop();
            // A função para parar o servidor httpd não é trivial, 
            // mas parar o Wi-Fi já torna o servidor inacessível.
            wifi_active = false;
        }
        vTaskDelay(pdMS_TO_TICKS(200)); // Verifica o botão a cada 200ms
    }
}

// Função que calcula o tempo e coloca o ESP32 para dormir
static void goToDeepSleep(void) {
    long sleep_duration_seconds;

    #ifdef MODO_DE_TESTE
        // MODO DE TESTE: Dorme por um tempo fixo e curto (60 segundos)
        sleep_duration_seconds = 60;
        ESP_LOGI(TAG, "TEST MODE: Sleeping for %ld seconds.", sleep_duration_seconds);
    #else
        // MODO NORMAL: Calcula o tempo até a próxima medição agendada
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        int next_minute = (timeinfo.tm_min < 30) ? 30 : 60;
        struct tm next_measurement_time = timeinfo;
        next_measurement_time.tm_min = next_minute;
        next_measurement_time.tm_sec = 0;

        if (next_minute == 60) {
            next_measurement_time.tm_hour += 1;
            next_measurement_time.tm_min = 0;
        }
        
        time_t next_measurement_seconds = mktime(&next_measurement_time);
        sleep_duration_seconds = next_measurement_seconds - now;

        if (sleep_duration_seconds < 5) sleep_duration_seconds = 30 * 60;

        ESP_LOGI(TAG, "Next measurement at %02d:%02d. Sleeping for %ld seconds.", 
                 next_measurement_time.tm_hour, next_measurement_time.tm_min, sleep_duration_seconds);
    #endif
    
    esp_sleep_enable_timer_wakeup(sleep_duration_seconds * 1000000);
    esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 1);
    
    ESP_LOGI(TAG, "Entering deep sleep...");
    // esp_log_wait_for_sent(); 
    esp_deep_sleep_start();
}


void app_main(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    
    // CASO 1: Acordou por causa do botão -> Ligar Wi-Fi
    if (cause == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "Woke up by button press. Activating Wi-Fi for 15 minutes...");
        nvs_flash_init();
        
        // --- CORREÇÃO 2: INICIALIZA O SD CARD ANTES DO SERVIDOR ---
        if (!init_sd_card()) {
            ESP_LOGE(TAG, "Failed to initialize SD card for HTTP Server.");
        } else {
            wifi_init_softap();
            start_http_server();
        }
        
        vTaskDelay(pdMS_TO_TICKS(300000)); // Fica acordado por 5 minutos
        ESP_LOGI(TAG, "Wi-Fi time finished. Going back to deep sleep.");
    } 
    // CASO 2: Acordou pelo timer ou foi ligado pela primeira vez -> Rotina de Medição
    else {
        nvs_flash_init();
        initialize_rtc();
        xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);      
        bool should_measure;
        #ifdef MODO_DE_TESTE
            // --- CORREÇÃO 1: FORÇA A MEDIÇÃO NO MODO DE TESTE ---
            should_measure = true;
            ESP_LOGI(TAG, "TEST MODE: Forcing measurement.");
        #else
            // MODO NORMAL: Verifica o horário
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            
            bool is_on_minute_schedule = (timeinfo.tm_min == 0 || timeinfo.tm_min == 30);
            bool is_in_hour_window = 
                (timeinfo.tm_hour >= 7 && timeinfo.tm_hour < 9) ||
                (timeinfo.tm_hour >= 11 && timeinfo.tm_hour < 13) ||
                (timeinfo.tm_hour >= 16 && timeinfo.tm_hour < 18) ||
                (timeinfo.tm_hour == 9 && timeinfo.tm_min == 0) ||
                (timeinfo.tm_hour == 13 && timeinfo.tm_min == 0) ||
                (timeinfo.tm_hour == 18 && timeinfo.tm_min == 0);
            should_measure = is_in_hour_window && is_on_minute_schedule;
        #endif

        if (should_measure) {
            ESP_LOGI(TAG, "Scheduled measurement time! Performing data acquisition.");
            if (init_sd_card()) {
                perform_single_measurement();
                close_current_file(); 
            } else {
                ESP_LOGE(TAG, "Failed to initialize SD card during measurement.");
            }
        } else {
            ESP_LOGI(TAG, "Woke up at non-scheduled time. Going back to sleep.");
        }
    }
    
    goToDeepSleep();
}
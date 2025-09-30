#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "dht.h"
#include "co2_sensor_task.h"
#include "sd_card.h"
#include "http_server.h"
#include "rtc.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

// *** MODO DE TESTE - comente para modo normal ***
#define MODO_DE_TESTE 

#define BUTTON_PIN GPIO_NUM_14
#define DHT_PIN GPIO_NUM_4 

static const char *TAG = "CO2-METER-MAIN";

// *** FUNÇÃO PARA CALCULAR PRÓXIMO DESPERTAR ***
static uint64_t calculate_next_wakeup_time(void) {
    #ifdef MODO_DE_TESTE
        // Modo teste: acordar a cada 60 segundos
        return 60 * 1000000ULL; // 60 segundos em microssegundos
    #else
        // Modo normal: acordar a cada 30 minutos
        return 30 * 60 * 1000000ULL; // 30 minutos em microssegundos
    #endif
}

// *** FUNÇÃO PARA VERIFICAR SE DEVE FAZER MEDIÇÃO ***
static bool should_perform_measurement(void) {
    #ifdef MODO_DE_TESTE
        // Modo teste: sempre fazer medição
        return true;
    #else
        // Modo normal: verificar horários específicos
        char date_str[11], time_str[9];
        get_current_date_time(date_str, time_str);
        
        if (strcmp(date_str, "ERRO-DATA") == 0) {
            return false; // RTC com problema
        }
        
        struct tm timeinfo;
        read_time_from_ds1302(&timeinfo);
        
        // Verificar se está nos horários: 07-09h, 11-13h, 16-18h
        int hour = timeinfo.tm_hour;
        bool in_time_window = (hour >= 7 && hour <= 9) ||
                             (hour >= 11 && hour <= 13) ||
                             (hour >= 16 && hour <= 18);
        
        // Verificar se é nos minutos 00 ou 30
        bool is_interval = (timeinfo.tm_min == 0) || (timeinfo.tm_min == 30);
        
        return in_time_window && is_interval;
    #endif
}

// *** FUNÇÃO PARA ENTRAR EM DEEP SLEEP ***
static void goToDeepSleep(void) {
    uint64_t wakeup_time = calculate_next_wakeup_time();
    
    ESP_LOGI(TAG, "Going to Deep Sleep for %llu seconds", wakeup_time / 1000000ULL);
    
    // Configurar despertar por timer
    esp_sleep_enable_timer_wakeup(wakeup_time);
    
    // Configurar despertar por botão (GPIO externo)
    esp_sleep_enable_ext0_wakeup(BUTTON_PIN, 1);
    
    // Entrar em Deep Sleep
    esp_deep_sleep_start();
}

// *** FUNÇÃO PARA INICIALIZAR WI-FI AP ***
static void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    (void)netif;

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32_CO2",
            .ssid_len = strlen("ESP32_CO2"),
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started. SSID: %s", wifi_config.ap.ssid);
}

// *** FUNÇÃO PRINCIPAL DO SISTEMA ***
void app_main() {
    // Inicializar NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Verificar causa do despertar
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    
    ESP_LOGI(TAG, "=== SYSTEM WAKE UP ===");
    
    switch (wakeup_reason) {
        case ESP_SLEEP_WAKEUP_EXT0:
            ESP_LOGI(TAG, "Wakeup caused by button press");
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI(TAG, "Wakeup caused by timer");
            break;
        default:
            ESP_LOGI(TAG, "Initial boot or other wakeup reason: %d", wakeup_reason);
            break;
    }

    // Inicializar componentes básicos
    ESP_LOGI(TAG, "Initializing SD Card...");
    if (!init_sd_card()) {
        ESP_LOGE(TAG, "SD Card initialization failed!");
        goToDeepSleep(); // Tentar novamente depois
        return;
    }

    ESP_LOGI(TAG, "Initializing RTC...");
    initialize_rtc();

    // Verificar se foi despertar por botão (modo Wi-Fi)
    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "=== WIFI MODE ACTIVATED ===");
        
        wifi_init_softap();
        start_http_server();
        
        ESP_LOGI(TAG, "WiFi server active for 15 minutes...");
        vTaskDelay(pdMS_TO_TICKS(15 * 60 * 1000)); // 15 minutos
        
        ESP_LOGI(TAG, "WiFi timeout - returning to sleep mode");
        esp_wifi_stop();
        goToDeepSleep();
        return;
    }
    
    // Modo normal: verificar se deve fazer medição
    if (should_perform_measurement()) {
        ESP_LOGI(TAG, "=== PERFORMING SCHEDULED MEASUREMENT ===");
        
        // Chamar função de medição única (não task contínua)
        perform_single_measurement();
        
        ESP_LOGI(TAG, "Measurement completed");
    } else {
        ESP_LOGI(TAG, "Outside measurement schedule - skipping");
    }
    
    // Sempre voltar para Deep Sleep após processar
    goToDeepSleep();
}
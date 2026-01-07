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
// esp_sleep.h não é necessário pois não usaremos modos de suspensão

//  #define BUTTON_PIN GPIO_NUM_14
#define DHT_PIN 4 

// #define MODO_DE_TESTE // Descomente para testes rápidos (medições a cada 30s)

static const char *TAG = "CO2-METER-INFERIOR";
static httpd_handle_t server_handle = NULL;

// --- FUNÇÃO DE INICIALIZAÇÃO DO WIFI ---
void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    (void)netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32_CO2_INFERIOR",
            .ssid_len = strlen("ESP32_CO2_INFERIOR"),
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK},
    };

    if (strlen((const char *)wifi_config.ap.password) == 0)
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    //sleep 3 segundos para garantir estabilidade
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Configurações para estabilidade e para MANTER O POWERBANK LIGADO
    esp_wifi_set_ps(WIFI_PS_NONE); // Desativa economia de energia
    esp_wifi_set_max_tx_power(78); // Potência máxima (19.5dBm)

    ESP_LOGI(TAG, "WiFi initialized. Power Save: OFF, TX Power: MAX.");
}

// --- TAREFA DE REDE (CORE 1) ---
static void network_task(void *arg)
{
    ESP_LOGI(TAG, "Starting Network Task on Core %d", xPortGetCoreID());

    wifi_init_softap();
    start_http_server(&server_handle);

    ESP_LOGI(TAG, "HTTP Server started.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100000)); 
    }
}

// --- TAREFA DE MEDIÇÃO (CORE 0) ---
static void measurement_scheduler_task(void *arg)
{
    ESP_LOGI(TAG, "Starting Scheduler Task on Core %d", xPortGetCoreID());

    // Variáveis para rastrear a ÚLTIMA vez que mediu
    // Inicializam com -1 para garantir que a primeira medição sempre ocorra
    static int last_meas_hour = -1;
    static int last_meas_min = -1;

    while (1)
    {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        ESP_LOGI(TAG, "Current Time: %02d:%02d:%02d (Core %d)", 
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, xPortGetCoreID());

#ifdef MODO_DE_TESTE
        bool should_measure = true;
        ESP_LOGI(TAG, "TEST MODE: Forcing measurement.");
#else
        // 1. Janela de operação (Dia/Noite)
        bool is_day_time =
            (timeinfo.tm_hour > 6 || (timeinfo.tm_hour == 6 && timeinfo.tm_min >= 30)) &&
            (timeinfo.tm_hour < 22 || (timeinfo.tm_hour == 22 && timeinfo.tm_min < 30));

        // 2. Janelas específicas
        bool is_in_measurement_window =
            (timeinfo.tm_hour >= 7 && timeinfo.tm_hour < 9) ||
            (timeinfo.tm_hour >= 11 && timeinfo.tm_hour < 13) ||
            (timeinfo.tm_hour >= 16 && timeinfo.tm_hour < 18) ||
            (timeinfo.tm_hour == 9 && timeinfo.tm_min == 0) ||
            (timeinfo.tm_hour == 13 && timeinfo.tm_min == 0) ||
            (timeinfo.tm_hour == 18 && timeinfo.tm_min == 0);

        // 3. Minuto exato
        bool is_on_minute_schedule = (timeinfo.tm_min == 0 || timeinfo.tm_min == 30);
        
        // 4. CORREÇÃO: Verifica se o horário atual é DIFERENTE do último medido
        bool is_new_time_slot = (timeinfo.tm_hour != last_meas_hour) || (timeinfo.tm_min != last_meas_min);

        // Só mede se todas as condições forem verdadeiras
        bool should_measure = is_day_time && is_in_measurement_window && is_on_minute_schedule && is_new_time_slot;
#endif

        if (should_measure)
        {
            ESP_LOGI(TAG, "Starting measurement cycle...");
            perform_single_measurement();
            close_current_file();
            
            // ATUALIZA O REGISTRO DA ÚLTIMA MEDIÇÃO
            last_meas_hour = timeinfo.tm_hour;
            last_meas_min = timeinfo.tm_min;
            
            ESP_LOGI(TAG, "Measurement recorded for slot %02d:%02d", last_meas_hour, last_meas_min);
        }
        
        // --- CÁLCULO DE ESPERA (DELAY) ---
#ifndef MODO_DE_TESTE
        // Atualiza a hora POIS a medição demorou
        time_t now_after;
        struct tm time_after;
        time(&now_after);
        localtime_r(&now_after, &time_after);

        int next_target_min = (time_after.tm_min < 30) ? 30 : 60;
        struct tm next_time_struct = time_after;
        next_time_struct.tm_min = (next_target_min == 60) ? 0 : 30;
        next_time_struct.tm_sec = 0;
        if (next_target_min == 60) {
            next_time_struct.tm_hour += 1;
        }

        time_t next_timestamp = mktime(&next_time_struct);
        long seconds_to_wait = (long)difftime(next_timestamp, now_after);

        // Proteção para não esperar tempo negativo ou muito curto
        if (seconds_to_wait < 5) seconds_to_wait = 60;

        ESP_LOGI(TAG, "Waiting %ld seconds for next slot.", seconds_to_wait);
        vTaskDelay(pdMS_TO_TICKS(seconds_to_wait * 1000));
#else
        vTaskDelay(pdMS_TO_TICKS(30000));
#endif
    }
}

void app_main(void)
{
    // 1. Inicializa NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. INICIALIZAÇÃO DE HARDWARE
    initialize_rtc();
    
    if (!init_sd_card()) {
        ESP_LOGE(TAG, "CRITICAL: Failed to initialize SD card in app_main!");
    } else {
        ESP_LOGI(TAG, "SD Card initialized successfully.");
    }

    // Liga energia do sensor (ajuda o powerbank)
    co2_sensor_power_control(true); 

    // 3. Criação das Tarefas
    xTaskCreatePinnedToCore(network_task, "NetworkTask", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(measurement_scheduler_task, "SchedulerTask", 8192, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "System started. Power Save OFF.");
}
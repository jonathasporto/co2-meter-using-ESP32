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

void app_main() {
    // Inicializa NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Inicializa o cartão SD
    // ESP_LOGI(TAG, "Initializing SD CARD...");
    // if (!init_sd_card()) {
    //     ESP_LOGE(TAG, "Failed to initialize SD card");
    //     return;
    // }

    // Inicializa o RTC (agora o DS1302)
    ESP_LOGI(TAG, "Initializing RTC CLOCK...");
    initialize_rtc();

    // --- INÍCIO DO BLOCO DE TESTE DE SENSORES ---
    ESP_LOGI(TAG, "--- SENSOR TEST ROUTINE START ---");
    vTaskDelay(pdMS_TO_TICKS(1000)); // Pequena pausa

    // 1. Teste do RTC
    char date_str[11], time_str[9];
    get_current_date_time(date_str, time_str);
    ESP_LOGI(TAG, "[TEST] RTC Date/Time: %s %s", date_str, time_str);

    // 2. Teste do DHT22
    float temperature = 0.0, humidity = 0.0;
    if (dht_read_float_data(DHT_TYPE_AM2301, 15, &humidity, &temperature) == ESP_OK) {
        ESP_LOGI(TAG, "[TEST] DHT22 Read: Temp=%.1fC, Hum=%.1f%%", temperature, humidity);
    } else {
        ESP_LOGE(TAG, "[TEST] DHT22 Read: FAILED");
    }
    ESP_LOGI(TAG, "--- SENSOR TEST ROUTINE END ---");
    // --- FIM DO BLOCO DE TESTE ---


    // Inicializa o Wi-Fi em modo AP
    ESP_LOGI(TAG, "Initializing Wifi AP MODE...");
    wifi_init_softap();

    // Inicializa o RTC
    ESP_LOGI(TAG, "Initializing RTC CLOCK...");
    initialize_rtc();

    // Inicia o servidor HTTP
    ESP_LOGI(TAG, "Initializing HTTP SERVER...");
    start_http_server();

    // Inicia a tarefa do sensor
    ESP_LOGI(TAG, "Initializing CO2 SENSOR TASK...");
    start_co2_sensor_task();
}

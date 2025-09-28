#include "co2_sensor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sd_card.h"
#include "rtc.h"
#include "dht.h" // Inclui a biblioteca do DHT

#define UART_PORT UART_NUM_1
#define TX_PIN 17
#define RX_PIN 16
#define DHT_PIN 4 
#define FAN_PIN 13 
#define UART_BUF_SIZE 1024

static const char *TAG = "CO2_SENSOR_TASK";

static void co2_sensor_task(void *arg) {
    // Configuração da UART (mesma do código anterior)
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_PORT, &uart_config);
    uart_set_pin(UART_PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_PORT, UART_BUF_SIZE, 0, 0, NULL, 0);

    gpio_reset_pin(FAN_PIN);
    gpio_set_direction(FAN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(FAN_PIN, 0); // Garante que comece desligado

    uint8_t read_cmd[9] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79 };
    uint8_t data[9];
    bool measurement_taken_for_this_slot = false;


    while (1) {

        // Obtém a hora atual do RTC
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        bool is_in_hour_window = (timeinfo.tm_hour >= 7 && timeinfo.tm_hour < 9) ||
                                 (timeinfo.tm_hour >= 11 && timeinfo.tm_hour < 13) ||
                                 (timeinfo.tm_hour >= 16 && timeinfo.tm_hour < 18);
        
        bool is_on_minute_schedule = (timeinfo.tm_min == 0 || timeinfo.tm_min == 30);

        if (is_in_hour_window && is_on_minute_schedule) {
            // Se a hora está correta e ainda não fizemos a medição para este horário
            if (!measurement_taken_for_this_slot) {
                ESP_LOGI(TAG, "Scheduled reading triggered at %02d:%02d.", timeinfo.tm_hour, timeinfo.tm_min);
                
                // 1. Liga o Fan para renovar o ar
                ESP_LOGI(TAG, "Activating fan for 30 seconds...");
                gpio_set_level(FAN_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(30000)); // Deixa o fan ligado por 30s

                // 2. Realiza a leitura dos sensores
                uart_write_bytes(UART_PORT, (const char *)read_cmd, sizeof(read_cmd));
                int len = uart_read_bytes(UART_PORT, data, sizeof(data), pdMS_TO_TICKS(1000));
                
                // 3. Desliga o fan
                gpio_set_level(FAN_PIN, 0);
                ESP_LOGI(TAG, "Deactivating fan.");

                if (len == 9) {
                    // Verifica o checksum
                    uint8_t checksum = 0;
                    for (int i = 1; i < 8; i++) {
                        checksum += data[i];
                    }
                    checksum = 0xFF - checksum + 1;

                    if (checksum == data[8]) {
                        int co2_concentration = (data[2] << 8) | data[3];

                        // NOVA PARTE: Leitura do sensor DHT22
                        float temperature = 0.0, humidity = 0.0;
                        if (dht_read_float_data(DHT_TYPE_AM2301, DHT_PIN, &humidity, &temperature) != ESP_OK) {
                            ESP_LOGE(TAG, "Could not read data from DHT22");
                        }

                        char date_str[11], time_str[9];
                        get_current_date_time(date_str, time_str);

                        // LOG ATUALIZADO: Mostra todos os dados
                        ESP_LOGI(TAG, "Date: %s Time: %s | CO2: %d ppm | Temp: %.1fC | Hum: %.1f%%", 
                                date_str, time_str, co2_concentration, temperature, humidity);

                        char csv_line[128];
                        snprintf(csv_line, sizeof(csv_line), "%s;%s;%d;%.1f;%.1f\n", 
                                date_str, time_str, co2_concentration, temperature, humidity);

                        write_data_to_csv(csv_line);

                    }
                
                // 5. Ativa a trava para não repetir a medição no mesmo minuto
                measurement_taken_for_this_slot = true;
            }
        } else {
            // Se saiu do horário de medição, reseta a trava para o próximo
            measurement_taken_for_this_slot = false;
        }

        vTaskDelay(pdMS_TO_TICKS(1000)); // Verifica a hora a cada segundo
        }
    }
}

void start_co2_sensor_task(void) {
    xTaskCreate(co2_sensor_task, "co2_sensor_task", 4096, NULL, 5, NULL);
}

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
#define DHT_PIN 15 // Pino do DHT22
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

    uint8_t read_cmd[9] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79 };
    uint8_t data[9];

    // Tempo de aquecimento para o sensor de CO2
    ESP_LOGI(TAG, "Waiting 3 minutes for MH-Z14A sensor to warm up...");
    vTaskDelay(pdMS_TO_TICKS(180)); // 3 minutos = 180.000 ms
    ESP_LOGI(TAG, "Sensor warm-up complete. Starting readings.");


    while (1) {
        // Envia o comando para ler a concentração de CO2
        uart_write_bytes(UART_PORT, (const char *)read_cmd, sizeof(read_cmd));

        // Aguarda a resposta do sensor
        int len = uart_read_bytes(UART_PORT, data, sizeof(data), pdMS_TO_TICKS(1000));

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

                // TODO: Descomentar a linha abaixo quando os resistores do SD card forem conectados
                // write_data_to_csv(csv_line);

            } else {
                ESP_LOGE(TAG, "Checksum error");
            }
        } else {
            ESP_LOGE(TAG, "Invalid data length");
        }

        vTaskDelay(pdMS_TO_TICKS(2000)); // Aguarda 2 segundos para a próxima leitura
    }
}

void start_co2_sensor_task(void) {
    xTaskCreate(co2_sensor_task, "co2_sensor_task", 4096, NULL, 5, NULL);
}

#include "co2_sensor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sd_card.h"
#include "rtc.h"

#define UART_PORT UART_NUM_1
#define TX_PIN 17
#define RX_PIN 16
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
                // Calcula a concentração de CO2
                int co2_concentration = (data[2] << 8) | data[3];

                // Obtém data e hora atuais
                char date_str[11]; // "YYYY-MM-DD"
                char time_str[9];  // "HH:MM:SS"
                get_current_date_time(date_str, time_str);

                ESP_LOGI(TAG, "Date: %s Time: %s CO2 Concentration: %d ppm", date_str, time_str, co2_concentration);

                // Formata a linha CSV
                char csv_line[64];
                snprintf(csv_line, sizeof(csv_line), "%s;%s;%d\n", date_str, time_str, co2_concentration);

                // Grava no arquivo CSV
                write_data_to_csv(csv_line);

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

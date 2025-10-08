#include "co2_sensor_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sd_card.h"
#include "rtc.h"
#include "dht.h"
#include <stdlib.h>
#include "esp_sleep.h"

#define FAN_PURGE_DURATION_S 9       // Duração que o fan fica ligado para limpeza, em segundos.
#define NUM_AMOSTRAS 31              // Número de amostras a serem coletadas para a mediana. Use um número ímpar.
#define INTERVALO_AMOSTRAS_MS 1000   // Intervalo entre cada amostra rápida em milissegundos.

#define UART_PORT UART_NUM_1
#define TX_PIN 17
#define RX_PIN 16
#define DHT_PIN 4 
#define FAN_PIN 13 
#define UART_BUF_SIZE 1024

static const char *TAG = "CO2_SENSOR_TASK";

// Função auxiliar para ordenar o array de amostras para o cálculo da mediana.
int comparar_inteiros(const void * a, const void * b) {
   return ( *(int*)a - *(int*)b );
}


void perform_single_measurement(void) {
    // --- Configuração dos Pinos e Periféricos ---
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

    ESP_LOGI(TAG, "Performing scheduled measurement...");
    
    // 1. Liga o Fan para renovar o ar
    ESP_LOGI(TAG, "Activating fan for %d seconds to purge air...", FAN_PURGE_DURATION_S);
    gpio_set_level(FAN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(FAN_PURGE_DURATION_S * 1000));

    // 2. Desliga o fan ANTES de iniciar as medições
    gpio_set_level(FAN_PIN, 0);
    ESP_LOGI(TAG, "Fan deactivated. Starting measurements in static air.");
    vTaskDelay(pdMS_TO_TICKS(1000)); // Pequena pausa para o ar assentar

    // 3. Leitura do DHT (já com o ar renovado e parado)
    float temperature = 0.0, humidity = 0.0;
    if (dht_read_float_data(DHT_TYPE_AM2301, DHT_PIN, &humidity, &temperature) != ESP_OK) {
        ESP_LOGE(TAG, "Could not read data from DHT22");
    }
    // 4. --- INÍCIO DA COLETA RÁPIDA DE AMOSTRAS ---
    ESP_LOGI(TAG, "Collecting %d CO2 samples...", NUM_AMOSTRAS);
    int co2_amostras[NUM_AMOSTRAS];
    int amostras_validas = 0;
    uint8_t read_cmd[9] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79 };
    
    for (int i = 0; i < NUM_AMOSTRAS; i++) {
        uart_write_bytes(UART_PORT, (const char *)read_cmd, sizeof(read_cmd));
        uint8_t data[9];
        int len = uart_read_bytes(UART_PORT, data, sizeof(data), pdMS_TO_TICKS(1000));

        if (len == 9) { // Checagem básica de recebimento
            co2_amostras[i] = (data[2] << 8) | data[3];
            amostras_validas++;
        } else {
            co2_amostras[i] = -1; // Marca como leitura inválida
        }
        vTaskDelay(pdMS_TO_TICKS(INTERVALO_AMOSTRAS_MS));
    }
    ESP_LOGI(TAG, "Sample collection finished. Valid samples: %d/%d", amostras_validas, NUM_AMOSTRAS);
    // --- FIM DA COLETA RÁPIDA DE AMOSTRAS ---

    // 5. --- CÁLCULO DA MEDIANA ---
    int co2_mediana = -1;
    if (amostras_validas > 0) {
        // Ordena o array de amostras do menor para o maior
        qsort(co2_amostras, NUM_AMOSTRAS, sizeof(int), comparar_inteiros);
        // O valor da mediana é o elemento do meio do array ordenado
        co2_mediana = co2_amostras[NUM_AMOSTRAS / 2];
    }
    // --- FIM DO CÁLCULO DA MEDIANA ---
    
    // 6. Processa e salva o valor final (a mediana)
    char date_str[11], time_str[9];
    get_current_date_time(date_str, sizeof(date_str), time_str, sizeof(time_str));

    ESP_LOGI(TAG, "FINAL VALUE: %s %s | CO2 (Median): %d ppm | Temp: %.1fC | Hum: %.1f%%", 
                date_str, time_str, co2_mediana, temperature, humidity);

    char csv_line[128];
    snprintf(csv_line, sizeof(csv_line), "%s;%s;%d;%.1f;%.1f\n", 
                date_str, time_str, co2_mediana, temperature, humidity);
    
    write_data_to_csv(csv_line);

    // Desinstala o driver da UART para economizar energia
    uart_driver_delete(UART_PORT);
    
}
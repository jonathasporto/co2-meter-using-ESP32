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
#include <string.h>

#define FAN_PURGE_DURATION_S 9       
#define NUM_AMOSTRAS 61              
#define INTERVALO_AMOSTRAS_MS 1000   

#define UART_PORT UART_NUM_1
#define TX_PIN GPIO_NUM_17
#define RX_PIN GPIO_NUM_16
#define DHT_PIN GPIO_NUM_4 
#define FAN_PIN GPIO_NUM_13 
#define UART_BUF_SIZE 1024

static const char *TAG = "CO2_MEASUREMENT";

// Função para ordenar amostras
static int comparar_inteiros(const void * a, const void * b) {
   return ( *(int*)a - *(int*)b );
}

// *** FUNÇÃO DE MEDIÇÃO ÚNICA (não task contínua) ***
void perform_single_measurement(void) {
    ESP_LOGI(TAG, "Starting single measurement procedure...");
    
    // Configuração UART para sensor CO2
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

    // Configuração do FAN
    gpio_reset_pin(FAN_PIN);
    gpio_set_direction(FAN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(FAN_PIN, 0);

    // 1. Ativar fan para purga de ar
    ESP_LOGI(TAG, "Step 1/5: Activating fan for %d seconds...", FAN_PURGE_DURATION_S);
    gpio_set_level(FAN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(FAN_PURGE_DURATION_S * 1000));
    
    // 2. Desativar fan e aguardar estabilização
    ESP_LOGI(TAG, "Step 2/5: Fan deactivated, waiting for air to settle...");
    gpio_set_level(FAN_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // 3. Leitura do DHT22
    ESP_LOGI(TAG, "Step 3/5: Reading DHT22 sensor...");
    float temperature = -999.0, humidity = -999.0;
    esp_err_t dht_result = dht_read_float_data(DHT_TYPE_AM2301, DHT_PIN, &humidity, &temperature);
    
    if (dht_result == ESP_OK) {
        ESP_LOGI(TAG, "DHT22 successful: %.1f°C, %.1f%%", temperature, humidity);
    } else {
        ESP_LOGW(TAG, "DHT22 failed: %d", dht_result);
    }
    
    // 4. Coleta de múltiplas amostras de CO2
    ESP_LOGI(TAG, "Step 4/5: Collecting %d CO2 samples...", NUM_AMOSTRAS);
    int co2_amostras[NUM_AMOSTRAS];
    int amostras_validas = 0;
    uint8_t read_cmd[9] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79 };
    
    for (int i = 0; i < NUM_AMOSTRAS; i++) {
        uart_write_bytes(UART_PORT, (const char *)read_cmd, sizeof(read_cmd));
        
        uint8_t data[9];
        int len = uart_read_bytes(UART_PORT, data, sizeof(data), pdMS_TO_TICKS(1000));
        
        if (len == 9 && data[0] == 0xFF && data[1] == 0x86) {
            int co2_value = (data[2] << 8) | data[3];
            if (co2_value >= 300 && co2_value <= 5000) {
                co2_amostras[amostras_validas] = co2_value;
                amostras_validas++;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(INTERVALO_AMOSTRAS_MS));
    }
    
    // 5. Calcular mediana e salvar dados
    ESP_LOGI(TAG, "Step 5/5: Processing and saving data...");
    int co2_mediana = -1;
    
    if (amostras_validas >= (NUM_AMOSTRAS / 2)) {
        qsort(co2_amostras, amostras_validas, sizeof(int), comparar_inteiros);
        co2_mediana = co2_amostras[amostras_validas / 2];
        ESP_LOGI(TAG, "CO2 median: %d ppm (%d valid samples)", co2_mediana, amostras_validas);
    } else {
        ESP_LOGE(TAG, "Insufficient valid samples: %d/%d", amostras_validas, NUM_AMOSTRAS);
    }
    
    // Obter timestamp atual
    char date_str[11], time_str[9];
    get_current_date_time(date_str, time_str);
    
    // Preparar dados CSV
    char csv_line[128];
    snprintf(csv_line, sizeof(csv_line), "%s;%s;%d;%.1f;%.1f\n", 
             date_str, time_str, co2_mediana, temperature, humidity);
    
    // Salvar no SD Card
    esp_err_t write_result = write_data_to_csv(csv_line);
    if (write_result == ESP_OK) {
        ESP_LOGI(TAG, "Data saved successfully to SD card");
    } else {
        ESP_LOGE(TAG, "Failed to save data: %s", esp_err_to_name(write_result));
    }
    
    // Limpeza
    uart_driver_delete(UART_PORT);
    gpio_reset_pin(FAN_PIN);
    
    ESP_LOGI(TAG, "=== MEASUREMENT COMPLETE ===");
}

// *** MANTER COMPATIBILIDADE COM CÓDIGO ANTIGO ***
void start_co2_sensor_task(void) {
    // Esta função agora apenas chama a medição única
    perform_single_measurement();
}
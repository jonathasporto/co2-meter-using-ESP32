#include "rtc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "DS1302_RTC";

// Pinos do DS1302
#define DS1302_CLK_PIN      GPIO_NUM_27
#define DS1302_IO_PIN       GPIO_NUM_26
#define DS1302_RST_PIN      GPIO_NUM_25

#define DS1302_SECONDS_REG      0x80
#define DS1302_WRITE_PROTECT    0x8E

// Funções de baixo nível para comunicação (bcd_to_dec, dec_to_bcd, etc.)
static uint8_t bcd_to_dec(uint8_t val) { return (val / 16 * 10) + (val % 16); }
static uint8_t dec_to_bcd(uint8_t val) { return (val / 10 * 16) + (val % 10); }

static void ds1302_write_byte(uint8_t value) {
    for (int i = 0; i < 8; i++) {
        gpio_set_level(DS1302_IO_PIN, (value >> i) & 1);
        gpio_set_level(DS1302_CLK_PIN, 1);
        esp_rom_delay_us(1);
        gpio_set_level(DS1302_CLK_PIN, 0);
        esp_rom_delay_us(1);
    }
}

static uint8_t ds1302_read_byte(void) {
    uint8_t value = 0;
    gpio_set_direction(DS1302_IO_PIN, GPIO_MODE_INPUT);
    for (int i = 0; i < 8; i++) {
        value |= (gpio_get_level(DS1302_IO_PIN) << i);
        gpio_set_level(DS1302_CLK_PIN, 1);
        esp_rom_delay_us(1);
        gpio_set_level(DS1302_CLK_PIN, 0);
        esp_rom_delay_us(1);
    }
    gpio_set_direction(DS1302_IO_PIN, GPIO_MODE_OUTPUT);
    return value;
}

static void ds1302_write_reg(uint8_t reg, uint8_t value) {
    gpio_set_level(DS1302_RST_PIN, 1);
    ds1302_write_byte(reg);
    ds1302_write_byte(value);
    gpio_set_level(DS1302_RST_PIN, 0);
}

static uint8_t ds1302_read_reg(uint8_t reg) {
    uint8_t value;
    gpio_set_level(DS1302_RST_PIN, 1);
    ds1302_write_byte(reg | 1);
    value = ds1302_read_byte();
    gpio_set_level(DS1302_RST_PIN, 0);
    return value;
}

// Função principal de inicialização (renomeada para manter compatibilidade)
void initialize_rtc(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DS1302_CLK_PIN) | (1ULL << DS1302_RST_PIN) | (1ULL << DS1302_IO_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(DS1302_CLK_PIN, 0);
    gpio_set_level(DS1302_RST_PIN, 0);
    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x00);

    ESP_LOGW(TAG, "Forcing RTC time set to compile time.");
    const char *compile_date = __DATE__; // "Mmm dd yyyy"
    const char *compile_time = __TIME__; // "hh:mm:ss"

    struct tm timeinfo = {0};
    sscanf(compile_time, "%d:%d:%d", &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec);
    char month_str[4];
    sscanf(compile_date, "%s %d %d", month_str, &timeinfo.tm_mday, &timeinfo.tm_year);
    timeinfo.tm_year -= 1900;

    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; i++) {
        if (strcmp(month_str, months[i]) == 0) {
            timeinfo.tm_mon = i;
            break;
        }
    }
    // --- INÍCIO DA CORREÇÃO DE ATRASO ---
    // Converte a hora da compilação para segundos
    time_t compile_time_seconds = mktime(&timeinfo);

    // Adiciona 90 segundos para compensar o tempo de compilação e flash
    compile_time_seconds += 90; 

    // Converte os segundos de volta para a estrutura de tempo
    struct tm adjusted_timeinfo;
    localtime_r(&compile_time_seconds, &adjusted_timeinfo);
    // --- FIM DA CORREÇÃO DE ATRASO ---
    
    // Agora, usa a hora AJUSTADA ('adjusted_timeinfo') para gravar no RTC
    ds1302_write_reg(0x8E, 0x00); // Disable write protect
    ds1302_write_reg(0x8C, dec_to_bcd(adjusted_timeinfo.tm_year - 100));
    ds1302_write_reg(0x8A, dec_to_bcd(adjusted_timeinfo.tm_wday + 1));
    ds1302_write_reg(0x88, dec_to_bcd(adjusted_timeinfo.tm_mon + 1));
    ds1302_write_reg(0x86, dec_to_bcd(adjusted_timeinfo.tm_mday));
    ds1302_write_reg(0x84, dec_to_bcd(adjusted_timeinfo.tm_hour));
    ds1302_write_reg(0x82, dec_to_bcd(adjusted_timeinfo.tm_min));
    ds1302_write_reg(0x80, dec_to_bcd(adjusted_timeinfo.tm_sec & 0x7F)); // Habilita o clock
    ds1302_write_reg(0x8E, 0x80); // Re-enable write protect

    ESP_LOGI(TAG, "DS1302 RTC time has been set with compensation.");
}

void get_current_date_time(char *date_str, char *time_str) {
    struct tm timeinfo = {0};
    timeinfo.tm_sec = bcd_to_dec(ds1302_read_reg(0x81));
    timeinfo.tm_min = bcd_to_dec(ds1302_read_reg(0x83));
    timeinfo.tm_hour = bcd_to_dec(ds1302_read_reg(0x85));
    timeinfo.tm_mday = bcd_to_dec(ds1302_read_reg(0x87));
    timeinfo.tm_mon = bcd_to_dec(ds1302_read_reg(0x89)) - 1;
    timeinfo.tm_year = bcd_to_dec(ds1302_read_reg(0x8D)) + 100;

    strftime(date_str, 11, "%Y-%m-%d", &timeinfo);
    strftime(time_str, 9, "%H:%M:%S", &timeinfo);
}

void get_current_date_time_filename(char *date_time_str) {
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(date_time_str, 20, "%Y-%m-%d_%H-%M-%S", &timeinfo); // YYYY-MM-DD_HH-MM-SS
}
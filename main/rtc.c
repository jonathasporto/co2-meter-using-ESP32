#include "rtc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "DS1302_RTC";

// Pinos do DS1302
#define DS1302_CLK_PIN      GPIO_NUM_27
#define DS1302_IO_PIN       GPIO_NUM_26
#define DS1302_RST_PIN      GPIO_NUM_25

// Registradores do DS1302
#define DS1302_SECONDS_REG      0x80
#define DS1302_WRITE_PROTECT    0x8E

#define DS1302_DELAY_US 10

// CORREÇÃO: Offset em segundos para compensar a diferença observada
// Baseado na observação: RTC está ~5 minutos atrasado
// Ajuste este valor conforme necessário
#define RTC_OFFSET_SECONDS 300  // 5 minutos = 300 segundos

static uint8_t bcd_to_dec(uint8_t val) { return (val / 16 * 10) + (val % 16); }
static uint8_t dec_to_bcd(uint8_t val) { return (val / 10 * 16) + (val % 10); }

static void ds1302_write_byte(uint8_t value) {
    gpio_set_direction(DS1302_IO_PIN, GPIO_MODE_OUTPUT);
    for (int i = 0; i < 8; i++) {
        gpio_set_level(DS1302_IO_PIN, (value >> i) & 1);
        gpio_set_level(DS1302_CLK_PIN, 1);
        esp_rom_delay_us(DS1302_DELAY_US);
        gpio_set_level(DS1302_CLK_PIN, 0);
        esp_rom_delay_us(DS1302_DELAY_US);
    }
}

static uint8_t ds1302_read_byte(void) {
    uint8_t value = 0;
    gpio_set_direction(DS1302_IO_PIN, GPIO_MODE_INPUT);
    for (int i = 0; i < 8; i++) {
        value |= (gpio_get_level(DS1302_IO_PIN) << i);
        gpio_set_level(DS1302_CLK_PIN, 1);
        esp_rom_delay_us(DS1302_DELAY_US);
        gpio_set_level(DS1302_CLK_PIN, 0);
        esp_rom_delay_us(DS1302_DELAY_US);
    }
    return value;
}

static void ds1302_write_reg(uint8_t reg, uint8_t value) {
    gpio_set_level(DS1302_RST_PIN, 1);
    esp_rom_delay_us(DS1302_DELAY_US);
    ds1302_write_byte(reg & 0xFE); 
    ds1302_write_byte(value);
    esp_rom_delay_us(DS1302_DELAY_US);
    gpio_set_level(DS1302_RST_PIN, 0);
}

static uint8_t ds1302_read_reg(uint8_t reg) {
    uint8_t value;
    gpio_set_level(DS1302_RST_PIN, 1);
    esp_rom_delay_us(DS1302_DELAY_US);
    ds1302_write_byte(reg | 1); 
    value = ds1302_read_byte();
    esp_rom_delay_us(DS1302_DELAY_US);
    gpio_set_level(DS1302_RST_PIN, 0);
    return value;
}

bool read_time_from_ds1302(struct tm *timeinfo) {
    if (!timeinfo) return false;

    uint8_t sec_reg = ds1302_read_reg(0x81);
    if (sec_reg & 0x80) {
        ESP_LOGE(TAG, "Clock Halt bit is set. RTC time is not reliable.");
        return false;
    }

    timeinfo->tm_sec  = bcd_to_dec(sec_reg & 0x7F);
    timeinfo->tm_min  = bcd_to_dec(ds1302_read_reg(0x83) & 0x7F);
    timeinfo->tm_hour = bcd_to_dec(ds1302_read_reg(0x85) & 0x3F);
    timeinfo->tm_mday = bcd_to_dec(ds1302_read_reg(0x87) & 0x3F);
    timeinfo->tm_mon  = bcd_to_dec(ds1302_read_reg(0x89) & 0x1F) - 1;
    timeinfo->tm_wday = bcd_to_dec(ds1302_read_reg(0x8B) & 0x07) - 1;
    timeinfo->tm_year = bcd_to_dec(ds1302_read_reg(0x8D)) + 100; 
    timeinfo->tm_isdst = -1;
    
    return true;
}

// Função para ler a hora do RTC com correção de offset
bool read_time_from_ds1302_corrected(struct tm *timeinfo) {
    if (!read_time_from_ds1302(timeinfo)) {
        return false;
    }
    
    // Aplicar correção de offset
    time_t rtc_time = mktime(timeinfo);
    rtc_time += RTC_OFFSET_SECONDS;  // Adicionar o offset de correção
    localtime_r(&rtc_time, timeinfo);
    
    ESP_LOGI(TAG, "Hora do RTC corrigida: %04d-%02d-%02d %02d:%02d:%02d", 
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    
    return true;
}

void set_time_on_ds1302(const struct tm *timeinfo) {
    // Aplicar offset reverso antes de gravar no RTC
    time_t adjusted_time = mktime((struct tm*)timeinfo);
    adjusted_time -= RTC_OFFSET_SECONDS;  // Subtrair o offset antes de gravar
    
    struct tm rtc_time;
    localtime_r(&adjusted_time, &rtc_time);
    
    ESP_LOGI(TAG, "Gravando no RTC (com offset): %04d-%02d-%02d %02d:%02d:%02d",
             rtc_time.tm_year + 1900, rtc_time.tm_mon + 1, rtc_time.tm_mday,
             rtc_time.tm_hour, rtc_time.tm_min, rtc_time.tm_sec);
    
    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));

    ds1302_write_reg(0x8C, dec_to_bcd(rtc_time.tm_year - 100));
    ds1302_write_reg(0x8A, dec_to_bcd(rtc_time.tm_wday + 1));
    ds1302_write_reg(0x88, dec_to_bcd(rtc_time.tm_mon + 1));
    ds1302_write_reg(0x86, dec_to_bcd(rtc_time.tm_mday));
    ds1302_write_reg(0x84, dec_to_bcd(rtc_time.tm_hour));
    ds1302_write_reg(0x82, dec_to_bcd(rtc_time.tm_min));
    ds1302_write_reg(0x80, dec_to_bcd(rtc_time.tm_sec) & 0x7F);

    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x80);
    ESP_LOGI(TAG, "RTC time set successfully with offset correction.");
}

void set_manual_time_rtc(int year, int month, int day, int hour, int minute, int second) {
    struct tm manual_time = {0};
    manual_time.tm_year = year - 1900;
    manual_time.tm_mon = month - 1;
    manual_time.tm_mday = day;
    manual_time.tm_hour = hour;
    manual_time.tm_min = minute;
    manual_time.tm_sec = second;
    
    time_t temp_time = mktime(&manual_time);
    localtime_r(&temp_time, &manual_time);
    
    ESP_LOGI(TAG, "Definindo hora manual: %04d-%02d-%02d %02d:%02d:%02d", 
             year, month, day, hour, minute, second);
    
    set_time_on_ds1302(&manual_time);
}

void initialize_rtc(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DS1302_CLK_PIN) | (1ULL << DS1302_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(DS1302_CLK_PIN, 0);
    gpio_set_level(DS1302_RST_PIN, 0);
    
    vTaskDelay(pdMS_TO_TICKS(100));

    if (ds1302_read_reg(0x81) & 0x80) {
        ESP_LOGW(TAG, "RTC clock halt detected. Setting current time.");
        // Definir uma hora atual (ajuste conforme necessário)
        // set_manual_time_rtc(2025, 10, 5, 5, 36, 0);
    }

    ESP_LOGI(TAG, "Synchronizing system time with RTC (with offset correction)...");
    struct tm timeinfo_from_rtc = {0};

    if (read_time_from_ds1302_corrected(&timeinfo_from_rtc)) {
        if (timeinfo_from_rtc.tm_year > 100) { 
            time_t t = mktime(&timeinfo_from_rtc);
            struct timeval now = { .tv_sec = t };
            settimeofday(&now, NULL);
            ESP_LOGI(TAG, "System time synchronized with corrected RTC time.");
        } else {
            ESP_LOGW(TAG, "RTC returned invalid year. System time NOT synchronized.");
        }
    } else {
        ESP_LOGE(TAG, "Failed to read valid time from RTC. System time NOT synchronized.");
    }
}

void get_current_date_time(char *date_str, size_t date_len, char *time_str, size_t time_len) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(date_str, date_len, "%Y-%m-%d", &timeinfo);
    strftime(time_str, time_len, "%H:%M:%S", &timeinfo);
}

void get_current_date_time_filename(char *date_time_str, size_t len) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    strftime(date_time_str, len, "%Y-%m-%d_%Hh%Mm", &timeinfo);
}

// Função para ajustar o offset se necessário
void adjust_rtc_offset(int offset_seconds) {
    ESP_LOGI(TAG, "Ajustando offset do RTC para %d segundos", offset_seconds);
    // Esta função pode ser usada para ajustar o RTC_OFFSET_SECONDS em tempo de execução
    // Por enquanto, apenas loga o valor. Para implementar completamente,
    // seria necessário tornar RTC_OFFSET_SECONDS uma variável global.
}

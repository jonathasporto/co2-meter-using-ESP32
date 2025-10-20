#include "rtc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "nvs.h"
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

// Chave NVS para verificar se já foi inicializado
#define NVS_NAMESPACE "rtc_config"
#define NVS_KEY_INITIALIZED "rtc_init"

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

void set_time_on_ds1302(const struct tm *timeinfo) {
    ESP_LOGI(TAG, "Gravando no RTC: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    
    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));

    ds1302_write_reg(0x8C, dec_to_bcd(timeinfo->tm_year - 100));
    ds1302_write_reg(0x8A, dec_to_bcd(timeinfo->tm_wday + 1));
    ds1302_write_reg(0x88, dec_to_bcd(timeinfo->tm_mon + 1));
    ds1302_write_reg(0x86, dec_to_bcd(timeinfo->tm_mday));
    ds1302_write_reg(0x84, dec_to_bcd(timeinfo->tm_hour));
    ds1302_write_reg(0x82, dec_to_bcd(timeinfo->tm_min));
    ds1302_write_reg(0x80, dec_to_bcd(timeinfo->tm_sec) & 0x7F);

    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x80);
    ESP_LOGI(TAG, "RTC time set successfully.");
}

// Função para verificar se é a primeira inicialização
bool is_first_boot(void) {
    // Verificar se acordou do deep sleep
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    if (wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "Acordou do deep sleep - não é primeira inicialização");
        return false;
    }
    
    // Verificar flag no NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao abrir NVS: %s", esp_err_to_name(err));
        return true; // Assumir primeira inicialização se não conseguir acessar NVS
    }
    
    uint8_t initialized = 0;
    size_t required_size = sizeof(initialized);
    err = nvs_get_blob(nvs_handle, NVS_KEY_INITIALIZED, &initialized, &required_size);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Primeira vez - marcar como inicializado
        initialized = 1;
        nvs_set_blob(nvs_handle, NVS_KEY_INITIALIZED, &initialized, sizeof(initialized));
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Primeira inicialização detectada");
        return true;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Não é primeira inicialização");
    return false;
}

// Função para definir hora baseada na compilação
void set_compile_time_to_rtc(void) {
    const char *compile_date = __DATE__; // "Mmm dd yyyy"
    const char *compile_time = __TIME__; // "hh:mm:ss"
    struct tm timeinfo = {0};

    // Parse da hora de compilação
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

    // Calcular o dia da semana
    time_t temp_time = mktime(&timeinfo);
    localtime_r(&temp_time, &timeinfo);
    
    ESP_LOGI(TAG, "Definindo hora de compilação: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    
    set_time_on_ds1302(&timeinfo);
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
    // Inicializar NVS se necessário
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DS1302_CLK_PIN) | (1ULL << DS1302_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(DS1302_CLK_PIN, 0);
    gpio_set_level(DS1302_RST_PIN, 0);
    
    vTaskDelay(pdMS_TO_TICKS(100));

    // Verificar se o RTC está parado (Clock Halt)
    bool rtc_halted = (ds1302_read_reg(0x81) & 0x80) != 0;
    
    // Verificar se é primeira inicialização
     bool first_boot = is_first_boot();
    // bool first_boot = true;         // descomentar caso queira sempre resetar o RTC
    
    if (rtc_halted || first_boot) {
        if (rtc_halted) {
            ESP_LOGW(TAG, "RTC clock halt detected.");
        }
        if (first_boot) {
            ESP_LOGI(TAG, "Primeira inicialização - usando hora de compilação.");
        }
        
        // Usar hora de compilação
        set_compile_time_to_rtc();
    } else {
        ESP_LOGI(TAG, "RTC já inicializado - mantendo hora atual.");
    }

    // Sincronizar sistema com RTC
    ESP_LOGI(TAG, "Synchronizing system time with RTC...");
    struct tm timeinfo_from_rtc = {0};

    if (read_time_from_ds1302(&timeinfo_from_rtc)) {
        if (timeinfo_from_rtc.tm_year > 100) { 
            time_t t = mktime(&timeinfo_from_rtc);
            struct timeval now = { .tv_sec = t };
            settimeofday(&now, NULL);
            ESP_LOGI(TAG, "System time synchronized with RTC.");
            
            char buffer[30];
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo_from_rtc);
            ESP_LOGI(TAG, "Current RTC time: %s", buffer);
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

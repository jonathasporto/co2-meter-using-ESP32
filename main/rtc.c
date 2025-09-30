#include "rtc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "rom/ets_sys.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "DS1302_RTC";

// Pinos do DS1302
#define DS1302_CLK_PIN      GPIO_NUM_27
#define DS1302_IO_PIN       GPIO_NUM_26
#define DS1302_RST_PIN      GPIO_NUM_25

// Registradores do DS1302
#define DS1302_SECONDS_REG      0x80
#define DS1302_MINUTES_REG      0x82
#define DS1302_HOURS_REG        0x84
#define DS1302_DATE_REG         0x86
#define DS1302_MONTH_REG        0x88
#define DS1302_DAY_REG          0x8A
#define DS1302_YEAR_REG         0x8C
#define DS1302_WRITE_PROTECT    0x8E

// Máscaras para validação
#define SECONDS_MASK    0x7F
#define MINUTES_MASK    0x7F
#define HOURS_MASK      0x3F
#define DATE_MASK       0x3F
#define MONTH_MASK      0x1F
#define YEAR_MASK       0xFF

static bool rtc_initialized = false;
static bool rtc_hardware_ok = false;

// *** DECLARAÇÕES DAS FUNÇÕES STATIC ***
static uint8_t ds1302_read_reg(uint8_t reg);
static void ds1302_write_reg(uint8_t reg, uint8_t value);
static uint8_t ds1302_read_byte(void);
static void ds1302_write_byte(uint8_t value);
static void delay_us(uint32_t us);
static bool is_valid_bcd(uint8_t val);
static uint8_t bcd_to_dec(uint8_t val);
static uint8_t dec_to_bcd(uint8_t val);
static void ds1302_gpio_init(void);
static bool test_rtc_connectivity(void);
static bool setup_default_time(void);

// *** IMPLEMENTAÇÕES DAS FUNÇÕES ***

// Delay microsegundo
static void delay_us(uint32_t us) {
    ets_delay_us(us);
}

// Funções de conversão BCD
static uint8_t bcd_to_dec(uint8_t val) {
    return ((val >> 4) * 10) + (val & 0x0F);
}

static uint8_t dec_to_bcd(uint8_t val) {
    return ((val / 10) << 4) + (val % 10);
}

// Validação BCD
static bool is_valid_bcd(uint8_t val) {
    uint8_t high = (val >> 4);
    uint8_t low = (val & 0x0F);
    return (high <= 9 && low <= 9);
}

// Inicialização robusta dos pinos
static void ds1302_gpio_init(void) {
    ESP_LOGI(TAG, "Configuring GPIO pins...");
    
    gpio_config_t io_conf = {};
    
    // RST pin (sempre output)
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << DS1302_RST_PIN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;  // Pull-up habilitado
    gpio_config(&io_conf);
    
    // CLK pin (sempre output)
    io_conf.pin_bit_mask = (1ULL << DS1302_CLK_PIN);
    gpio_config(&io_conf);
    
    // Estado inicial seguro
    gpio_set_level(DS1302_RST_PIN, 0);
    gpio_set_level(DS1302_CLK_PIN, 0);
    
    // IO pin (será alternado entre input/output)
    gpio_set_direction(DS1302_IO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(DS1302_IO_PIN, GPIO_PULLUP_ONLY);
    gpio_set_level(DS1302_IO_PIN, 0);
    
    delay_us(100); // Tempo para estabilização
    
    ESP_LOGI(TAG, "GPIO configured: RST=%d, CLK=%d, IO=%d", 
             DS1302_RST_PIN, DS1302_CLK_PIN, DS1302_IO_PIN);
}

// Escrita de byte melhorada
static void ds1302_write_byte(uint8_t value) {
    gpio_set_direction(DS1302_IO_PIN, GPIO_MODE_OUTPUT);
    
    for (int i = 0; i < 8; i++) {
        // Configurar dados
        gpio_set_level(DS1302_IO_PIN, (value >> i) & 1);
        delay_us(2);
        
        // Clock alto
        gpio_set_level(DS1302_CLK_PIN, 1);
        delay_us(2);
        
        // Clock baixo
        gpio_set_level(DS1302_CLK_PIN, 0);
        delay_us(2);
    }
}

// Leitura de byte melhorada
static uint8_t ds1302_read_byte(void) {
    uint8_t value = 0;
    
    gpio_set_direction(DS1302_IO_PIN, GPIO_MODE_INPUT);
    delay_us(2);
    
    for (int i = 0; i < 8; i++) {
        // Clock alto
        gpio_set_level(DS1302_CLK_PIN, 1);
        delay_us(2);
        
        // Ler dados
        if (gpio_get_level(DS1302_IO_PIN)) {
            value |= (1 << i);
        }
        
        // Clock baixo
        gpio_set_level(DS1302_CLK_PIN, 0);
        delay_us(2);
    }
    
    return value;
}

// Leitura de registrador
static uint8_t ds1302_read_reg(uint8_t reg) {
    // Início da transação
    gpio_set_level(DS1302_RST_PIN, 1);
    delay_us(4);
    
    // Enviar comando de leitura
    ds1302_write_byte(reg | 1);
    
    // Ler dados
    uint8_t value = ds1302_read_byte();
    
    // Fim da transação
    gpio_set_level(DS1302_RST_PIN, 0);
    delay_us(4);
    
    return value;
}

// Escrita em registrador
static void ds1302_write_reg(uint8_t reg, uint8_t value) {
    // Início da transação
    gpio_set_level(DS1302_RST_PIN, 1);
    delay_us(4);
    
    // Enviar comando de escrita
    ds1302_write_byte(reg);
    
    // Enviar dados
    ds1302_write_byte(value);
    
    // Fim da transação
    gpio_set_level(DS1302_RST_PIN, 0);
    delay_us(4);
}

// Teste de conectividade do RTC
static bool test_rtc_connectivity(void) {
    ESP_LOGI(TAG, "Testing RTC connectivity...");
    
    // Teste 1: Ler registrador de proteção contra escrita
    uint8_t wp_reg = ds1302_read_reg(DS1302_WRITE_PROTECT);
    ESP_LOGI(TAG, "Write Protection Register: 0x%02X", wp_reg);
    
    // Teste 2: Desabilitar proteção e tentar escrever/ler um valor teste
    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x00);
    
    // Salvar valor original dos segundos
    uint8_t original_sec = ds1302_read_reg(DS1302_SECONDS_REG);
    ESP_LOGI(TAG, "Original seconds register: 0x%02X", original_sec);
    
    // Teste de escrita/leitura (usar registrador de proteção como teste)
    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x55);
    uint8_t test_read = ds1302_read_reg(DS1302_WRITE_PROTECT);
    
    bool connectivity_ok = (test_read == 0x55);
    ESP_LOGI(TAG, "Connectivity test: %s (wrote 0x55, read 0x%02X)", 
             connectivity_ok ? "PASS" : "FAIL", test_read);
    
    // Restaurar proteção
    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x80);
    
    return connectivity_ok;
}

// Configuração inicial padrão do RTC
static bool setup_default_time(void) {
    ESP_LOGW(TAG, "Setting up default time (2024-01-01 12:00:00)");
    
    // Desabilitar proteção
    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Configurar data/hora padrão
    ds1302_write_reg(DS1302_SECONDS_REG, dec_to_bcd(0));   // 00 segundos
    ds1302_write_reg(DS1302_MINUTES_REG, dec_to_bcd(0));   // 00 minutos  
    ds1302_write_reg(DS1302_HOURS_REG, dec_to_bcd(12));    // 12 horas
    ds1302_write_reg(DS1302_DATE_REG, dec_to_bcd(1));      // Dia 1
    ds1302_write_reg(DS1302_MONTH_REG, dec_to_bcd(1));     // Janeiro
    ds1302_write_reg(DS1302_YEAR_REG, dec_to_bcd(24));     // 2024
    
    // Habilitar proteção
    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x80);
    
    vTaskDelay(pdMS_TO_TICKS(50)); // Aguardar estabilização
    
    // Verificar se a configuração funcionou
    uint8_t test_hour = ds1302_read_reg(DS1302_HOURS_REG) & HOURS_MASK;
    bool setup_ok = (bcd_to_dec(test_hour) == 12);
    
    ESP_LOGI(TAG, "Default time setup: %s", setup_ok ? "SUCCESS" : "FAILED");
    return setup_ok;
}

// *** FUNÇÕES PÚBLICAS ***

// Inicialização do RTC
void initialize_rtc(void) {
    ESP_LOGI(TAG, "Initializing DS1302 RTC...");
    
    // Configurar pinos GPIO
    ds1302_gpio_init();
    
    // Testar conectividade
    rtc_hardware_ok = test_rtc_connectivity();
    
    if (!rtc_hardware_ok) {
        ESP_LOGE(TAG, "RTC hardware communication failed!");
        ESP_LOGE(TAG, "Check connections: RST->%d, CLK->%d, IO->%d", 
                 DS1302_RST_PIN, DS1302_CLK_PIN, DS1302_IO_PIN);
        rtc_initialized = false;
        return;
    }
    
    // Verificar se o relógio está parado
    uint8_t seconds_reg = ds1302_read_reg(DS1302_SECONDS_REG);
    if (seconds_reg & 0x80) {
        ESP_LOGW(TAG, "Clock halt detected (CH bit set)");
        setup_default_time();
    }
    
    // Verificar dados válidos
    uint8_t hour = ds1302_read_reg(DS1302_HOURS_REG) & HOURS_MASK;
    uint8_t minute = ds1302_read_reg(DS1302_MINUTES_REG) & MINUTES_MASK;
    
    if (!is_valid_bcd(hour) || !is_valid_bcd(minute)) {
        ESP_LOGW(TAG, "Invalid BCD data detected, setting default time");
        setup_default_time();
    }
    
    rtc_initialized = true;
    ESP_LOGI(TAG, "DS1302 RTC initialization complete");
    
    // Mostrar hora atual
    char date_str[11], time_str[9];
    get_current_date_time(date_str, time_str);
    ESP_LOGI(TAG, "Current RTC time: %s %s", date_str, time_str);
}

// Leitura do RTC com fallback
void read_time_from_ds1302(struct tm *timeinfo) {
    if (!timeinfo) {
        ESP_LOGE(TAG, "Null timeinfo pointer");
        return;
    }
    
    // Se o hardware não está OK, usar tempo padrão
    if (!rtc_hardware_ok || !rtc_initialized) {
        ESP_LOGW(TAG, "RTC not available, using default time");
        timeinfo->tm_year = 124;  // 2024
        timeinfo->tm_mon = 0;     // Janeiro
        timeinfo->tm_mday = 1;    // Dia 1
        timeinfo->tm_hour = 12;   // 12:00:00
        timeinfo->tm_min = 0;
        timeinfo->tm_sec = 0;
        timeinfo->tm_isdst = -1;
        return;
    }
    
    // Ler registradores do RTC
    uint8_t sec = ds1302_read_reg(DS1302_SECONDS_REG) & SECONDS_MASK;
    uint8_t min = ds1302_read_reg(DS1302_MINUTES_REG) & MINUTES_MASK;
    uint8_t hour = ds1302_read_reg(DS1302_HOURS_REG) & HOURS_MASK;
    uint8_t date = ds1302_read_reg(DS1302_DATE_REG) & DATE_MASK;
    uint8_t month = ds1302_read_reg(DS1302_MONTH_REG) & MONTH_MASK;
    uint8_t year = ds1302_read_reg(DS1302_YEAR_REG) & YEAR_MASK;
    
    // Validar dados BCD
    if (!is_valid_bcd(sec) || !is_valid_bcd(min) || !is_valid_bcd(hour) ||
        !is_valid_bcd(date) || !is_valid_bcd(month) || !is_valid_bcd(year)) {
        ESP_LOGW(TAG, "Invalid BCD data, using previous valid time or default");
        // Usar tempo padrão em caso de erro
        timeinfo->tm_year = 124;  // 2024
        timeinfo->tm_mon = 0;     // Janeiro
        timeinfo->tm_mday = 1;    // Dia 1
        timeinfo->tm_hour = 12;   // 12:00:00
        timeinfo->tm_min = 0;
        timeinfo->tm_sec = 0;
        timeinfo->tm_isdst = -1;
        return;
    }
    
    // Converter para struct tm
    timeinfo->tm_sec = bcd_to_dec(sec);
    timeinfo->tm_min = bcd_to_dec(min);
    timeinfo->tm_hour = bcd_to_dec(hour);
    timeinfo->tm_mday = bcd_to_dec(date);
    timeinfo->tm_mon = bcd_to_dec(month) - 1; // Mês base 0
    timeinfo->tm_year = bcd_to_dec(year) + 100; // Ano base 1900
    timeinfo->tm_isdst = -1;
}

// Funções auxiliares
void get_current_date_time(char *date_str, char *time_str) {
    struct tm timeinfo;
    read_time_from_ds1302(&timeinfo);
    
    // Validação básica
    if (timeinfo.tm_year < 100 || timeinfo.tm_year > 199 ||
        timeinfo.tm_mon < 0 || timeinfo.tm_mon > 11 ||
        timeinfo.tm_mday < 1 || timeinfo.tm_mday > 31) {
        strcpy(date_str, "2024-01-01");  // Data padrão em vez de erro
        strcpy(time_str, "12:00:00");    // Hora padrão em vez de erro
        return;
    }
    
    strftime(date_str, 11, "%Y-%m-%d", &timeinfo);
    strftime(time_str, 9, "%H:%M:%S", &timeinfo);
}

void get_current_date_time_filename(char *date_time_str) {
    struct tm timeinfo;
    read_time_from_ds1302(&timeinfo);
    
    // Validação básica com fallback
    if (timeinfo.tm_year < 100 || timeinfo.tm_year > 199) {
        strcpy(date_time_str, "20240101_120000");
        return;
    }
    
    strftime(date_time_str, 20, "%Y%m%d_%H%M%S", &timeinfo);
}

bool rtc_set_time(int year, int month, int day, int hour, int minute, int second) {
    if (!rtc_hardware_ok) {
        ESP_LOGE(TAG, "Cannot set time - RTC hardware not available");
        return false;
    }
    
    ESP_LOGI(TAG, "Setting RTC time to: %04d-%02d-%02d %02d:%02d:%02d", 
             year, month, day, hour, minute, second);
    
    // Desabilitar proteção
    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Parar o relógio
    ds1302_write_reg(DS1302_SECONDS_REG, 0x80);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Escrever dados
    ds1302_write_reg(DS1302_MINUTES_REG, dec_to_bcd(minute));
    ds1302_write_reg(DS1302_HOURS_REG, dec_to_bcd(hour));
    ds1302_write_reg(DS1302_DATE_REG, dec_to_bcd(day));
    ds1302_write_reg(DS1302_MONTH_REG, dec_to_bcd(month));
    ds1302_write_reg(DS1302_YEAR_REG, dec_to_bcd(year - 2000));
    
    // Reiniciar o relógio
    ds1302_write_reg(DS1302_SECONDS_REG, dec_to_bcd(second));
    
    // Habilitar proteção
    ds1302_write_reg(DS1302_WRITE_PROTECT, 0x80);
    
    vTaskDelay(pdMS_TO_TICKS(50)); // Aguardar estabilização
    
    ESP_LOGI(TAG, "RTC time set successfully");
    return true;
}

bool rtc_sync_from_system(void) {
    ESP_LOGW(TAG, "System time sync not implemented in this version");
    return false;
}
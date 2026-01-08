#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h> // Necessário para função stat()
#include "sd_card.h"
#include "esp_log.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "rtc.h"

static const char *TAG = "SD_CARD";

#define MOUNT_POINT     "/sdcard"
#define SPI_DMA_CHAN    SDSPI_DEFAULT_DMA
#define FILE_PATH_MAX   128
#define PIN_NUM_MISO    GPIO_NUM_19
#define PIN_NUM_MOSI    GPIO_NUM_21
#define PIN_NUM_CLK     GPIO_NUM_18
#define PIN_NUM_CS      GPIO_NUM_5

static sdmmc_card_t *card;
static FILE *csv_file = NULL;
static time_t file_start_time = 0;

bool init_sd_card(void) {
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing SD card");

    // Configuração do host SPI
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    //host.slot = SPI2_HOST; // ou SPI3_HOST dependendo do seu hardware

    // Configuração do barramento SPI
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1, // Não utilizado
        .quadhd_io_num = -1, // Não utilizado
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return false;
    }

    // Configuração do dispositivo SD SPI
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    // Opções do sistema de arquivos FAT
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // Monta o sistema de arquivos FAT no cartão SD
    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set format_if_mount_failed = true.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        spi_bus_free(host.slot);
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted successfully");
    sdmmc_card_print_info(stdout, card);
    return true;
}

void get_daily_filename(char *filename, size_t len, const char *estrato) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    // Formato: /sdcard/2026-01-08-estrato.csv
    snprintf(filename, len, MOUNT_POINT"/%04d-%02d-%02d-%s.csv", 
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, estrato);
}

void write_data_to_csv(const char *data, const char *estrato) {
    char filepath[128];
    get_daily_filename(filepath, sizeof(filepath), estrato);

    // Verifica se o arquivo existe para decidir se escreve o cabeçalho
    struct stat st;
    bool file_exists = (stat(filepath, &st) == 0);

    ESP_LOGI(TAG, "Appending data to file: %s", filepath);
    
    // Abre em modo "a" (append) - adiciona ao final
    FILE *f = fopen(filepath, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for appending");
        return;
    }

    // Se é um arquivo novo (primeira medição do dia), cria o cabeçalho
    if (!file_exists) {
        fprintf(f, "Date;Time;CO2_PPM;Temperatura;Umidade;Estrato;Turno_Medicao\n");
    }

    // Escreve os dados
    fprintf(f, "%s", data);
    
    // Fecha imediatamente para garantir a gravação no cartão
    fclose(f);
    ESP_LOGI(TAG, "Data saved successfully.");
}

void close_current_file(void) {
    if (csv_file != NULL) {
        fclose(csv_file);
        csv_file = NULL;
    }   
}

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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

    // Configuração do barramento SPI
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
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

static esp_err_t open_new_csv_file(void) {  // *** ALTERAR PARA esp_err_t ***
    if (csv_file != NULL) {
        fclose(csv_file);
        csv_file = NULL;
    }

    // Obtém a data e hora atuais para nomear o arquivo
    char date_time_str[20];
    get_current_date_time_filename(date_time_str);

    // Salva o tempo de início do arquivo
    file_start_time = time(NULL);

    // Cria o caminho completo do arquivo
    char file_path[FILE_PATH_MAX];
    snprintf(file_path, sizeof(file_path), MOUNT_POINT"/%s.csv", date_time_str);

    // Abre o arquivo para escrita
    ESP_LOGI(TAG, "Opening file %s", file_path);
    csv_file = fopen(file_path, "w");
    if (csv_file == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: errno %d", errno);
        return ESP_FAIL;  // *** RETORNAR ERRO ***
    } else {
        ESP_LOGI(TAG, "Opened file: %s", file_path);
        // Escreve o cabeçalho do CSV
        fprintf(csv_file, "Date;Time;CO2_PPM;Temperatura;Umidade\n");
        fflush(csv_file);
        return ESP_OK;  // *** RETORNAR SUCESSO ***
    }
}

esp_err_t write_data_to_csv(const char *data) {  // *** ALTERAR PARA esp_err_t ***
    if (csv_file == NULL) {
        esp_err_t ret = open_new_csv_file();
        if (ret != ESP_OK) {
            return ret;  // *** RETORNAR ERRO SE FALHOU ***
        }
    }

    // Verifica se já passou 1 minuto para criar um novo arquivo (temporariamente)
    time_t current_time = time(NULL);
    if (difftime(current_time, file_start_time) >= 60) {
        esp_err_t ret = open_new_csv_file();
        if (ret != ESP_OK) {
            return ret;  // *** RETORNAR ERRO SE FALHOU ***
        }
    }

    if (csv_file != NULL) {
        int result = fprintf(csv_file, "%s", data);
        if (result < 0) {
            ESP_LOGE(TAG, "Failed to write data to file");
            return ESP_FAIL;  // *** RETORNAR ERRO ***
        }
        
        if (fflush(csv_file) != 0) {
            ESP_LOGE(TAG, "Failed to flush data to file");
            return ESP_FAIL;  // *** RETORNAR ERRO ***
        }
        
        ESP_LOGI(TAG, "Data successfully written to SD card.");
        return ESP_OK;  // *** RETORNAR SUCESSO ***
    }
    
    return ESP_FAIL;  // *** RETORNAR ERRO SE csv_file FOR NULL ***
}

void close_current_file(void) {
    if (csv_file != NULL) {
        fclose(csv_file);
        csv_file = NULL;
    }
}

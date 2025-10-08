#include <string.h>
#include <sys/dirent.h>
#include "http_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "sd_card.h"
#include "rtc.h"

static const char *TAG = "HTTP_SERVER";

#define MOUNT_POINT "/sdcard"
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_HTTPD_MAX_URI_LEN)
#define MAX_FILENAME_LEN 128

// Função file_get_handler atualizada
static esp_err_t file_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "file_get_handler called for URI: %s", req->uri);

    char filepath[FILE_PATH_MAX];

    // Constrói o caminho completo do arquivo
    snprintf(filepath, sizeof(filepath), MOUNT_POINT"%s", req->uri);

    FILE *file = fopen(filepath, "r");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    // Define o tipo de conteúdo e o cabeçalho Content-Disposition
    httpd_resp_set_type(req, "application/octet-stream");

    // Extrai o nome do arquivo a partir da URI
    const char *filename = strrchr(req->uri, '/');
    if (filename == NULL) {
        filename = req->uri;
    } else {
        filename++; // Pula a barra '/'
    }

    // Limitar o tamanho do filename para evitar truncagem
    static char filename_buf[MAX_FILENAME_LEN];
    strncpy(filename_buf, filename, MAX_FILENAME_LEN - 1);
    filename_buf[MAX_FILENAME_LEN - 1] = '\0'; // Garante terminação em nulo

    // Reduzir o tamanho do buffer 'disposition'
    char disposition[256];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename_buf);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    // Tornar o buffer 'chunk' estático para evitar alocação na pilha
    static char chunk[1024];
    size_t chunksize;
    do {
        chunksize = fread(chunk, 1, sizeof(chunk), file);
        if (chunksize > 0) {
            if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
                fclose(file);
                ESP_LOGE(TAG, "File sending failed!");
                httpd_resp_sendstr_chunk(req, NULL);
                return ESP_FAIL;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Pequena pausa para não sufocar o sistema
    } while (chunksize > 0);

    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// Manipulador para deletar arquivos
static esp_err_t file_delete_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "file_delete_handler called for URI: %s", req->uri);

    char filepath[FILE_PATH_MAX];

    // Extrai o nome do arquivo a partir da URI, removendo '/delete/'
    const char *filename = req->uri + strlen("/delete/");

    // Remove qualquer parte após '?'
    char filename_clean[MAX_FILENAME_LEN];
    strncpy(filename_clean, filename, MAX_FILENAME_LEN - 1);
    filename_clean[MAX_FILENAME_LEN - 1] = '\0';

    char *question_mark = strchr(filename_clean, '?');
    if (question_mark != NULL) {
        *question_mark = '\0'; // Trunca a string no '?'
    }

    // Verifica se o filename é válido
    if (filename_clean[0] == '\0' || strstr(filename_clean, "..")) {
        ESP_LOGE(TAG, "Invalid filename: %s", filename_clean);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    // Verifica se o tamanho do caminho não excede o máximo permitido
    if (strlen(MOUNT_POINT) + 1 + strlen(filename_clean) + 1 > sizeof(filepath)) {
        ESP_LOGE(TAG, "File path too long");
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "File path too long");
        return ESP_FAIL;
    }

    // Constrói o caminho completo do arquivo
    snprintf(filepath, sizeof(filepath), MOUNT_POINT"/%s", filename_clean);

    // Log para depuração
    ESP_LOGI(TAG, "Attempting to delete file: %s", filepath);

    // Tenta excluir o arquivo
    if (remove(filepath) == 0) {
        ESP_LOGI(TAG, "Deleted file: %s", filepath);
        // Redireciona de volta para a lista de arquivos
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
    } else {
        ESP_LOGE(TAG, "Failed to delete file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }
    return ESP_OK;
}

// Manipulador para interface com o usuario
// Cabeçalho HTML com estilos e scripts
static const char *HTML_HEADER = "<!DOCTYPE html>"
"<html lang=\"pt-BR\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
"<title>CO₂ Level Monitor</title>"
"<style>"
"body { font-family: Arial, sans-serif; background-color: #f0f0f0; margin: 0; padding: 0; }"
"header { background-color: blue; color: white; padding: 20px; text-align: center; }"
"main { padding: 20px; }"
"table { width: 100%; border-collapse: collapse; }"
"th, td { padding: 12px; text-align: left; border-bottom: 1px solid #ddd; }"
"tr:hover { background-color: #f5f5f5; }"
"a { color: blue; text-decoration: none; }"
"a:hover { text-decoration: underline; }"
".btn { background-color: #f44336; color: white; padding: 8px 16px; text-align: center; border: none; border-radius: 4px; cursor: pointer; }"
".btn:hover { background-color: #d32f2f; }"
".btn-download { background-color: #4CAF50; color: white; padding: 8px 16px; border: none; border-radius: 4px; cursor: pointer; }"
".btn-download:hover { background-color: #45a049; }"
"</style>"
"</head>"
"<body>"
"<header>"
"<h1>CO₂ Level Monitor</h1>"
"</header>"
"<main>";
static const char *HTML_FOOTER = "</main></body></html>";

static esp_err_t file_list_handler(httpd_req_t *req) {
    struct dirent *entry;
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open directory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HTML_HEADER);

    // Obter a data e hora atuais usando o RTC
    char date_str[11]; // Formato YYYY-MM-DD
    char time_str[9];  // Formato HH:MM:SS
    get_current_date_time(date_str, sizeof(date_str), time_str, sizeof(time_str));

    // Formatar a data e hora em uma única string
    char datetime_str[32];
    snprintf(datetime_str, sizeof(datetime_str), "%s %s", date_str, time_str);

    // Inserir a data e hora na página
    char datetime_html[128];
    snprintf(datetime_html, sizeof(datetime_html),
             "<p><strong>Data e Hora Atuais:</strong> %s</p>", datetime_str);
    httpd_resp_sendstr_chunk(req, datetime_html);

    httpd_resp_sendstr_chunk(req, "<h2>Arquivos CSV Disponíveis</h2>");
    httpd_resp_sendstr_chunk(req, "<table>");
    httpd_resp_sendstr_chunk(req, "<tr><th>Nome do Arquivo</th><th>Ações</th></tr>");

    // Tornar 'line' estático para reduzir o uso da pilha
    static char line[512];

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char filename[MAX_FILENAME_LEN];
            strncpy(filename, entry->d_name, MAX_FILENAME_LEN - 1);
            filename[MAX_FILENAME_LEN - 1] = '\0';

            int ret = snprintf(line, sizeof(line),
                "<tr>"
                "<td><a href=\"/%s\">%s</a></td>"
                "<td>"
                "<a href=\"/%s\"><button class=\"btn-download\">Download</button></a> "
                "<form method=\"GET\" action=\"/delete/%s\" onsubmit=\"return confirm('Tem certeza que deseja excluir este arquivo?');\" style=\"display:inline;\">"
                "<button type=\"submit\" class=\"btn\">Excluir</button>"
                "</form>"
                "</td>"
                "</tr>",
                filename, filename, filename, filename);
                
            if (ret >= sizeof(line)) {
                ESP_LOGW(TAG, "Filename too long, skipping");
                continue;
            }

            httpd_resp_sendstr_chunk(req, line);
        }
    }
    closedir(dir);
    httpd_resp_sendstr_chunk(req, "</table>");
    httpd_resp_sendstr_chunk(req, HTML_FOOTER);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// Manipulador para favicon.ico
static esp_err_t favicon_get_handler(httpd_req_t *req) {
    httpd_resp_send(req, NULL, 0); // Retorna 0 bytes, indicando que não há conteúdo
    return ESP_OK;
}


// Função para iniciar o servidor HTTP
void start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Habilitar correspondência de URI com curingas
    config.uri_match_fn = httpd_uri_match_wildcard;

    // Inicia o servidor HTTP
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        // Registro do manipulador para favicon.ico
        httpd_uri_t favicon_handler = {
            .uri       = "/favicon.ico",
            .method    = HTTP_GET,
            .handler   = favicon_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &favicon_handler);

        // Registro do manipulador para listar arquivos
        httpd_uri_t file_list = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = file_list_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &file_list);

        // Registro do manipulador para deletar arquivos
        httpd_uri_t file_delete = {
            .uri       = "/delete/*",
            .method    = HTTP_GET, // Usando método GET
            .handler   = file_delete_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &file_delete);

        // Registro do manipulador para servir arquivos
        httpd_uri_t file_download = {
            .uri       = "/*",
            .method    = HTTP_GET,
            .handler   = file_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &file_download);

        ESP_LOGI(TAG, "HTTP server started");
    } else {
        ESP_LOGE(TAG, "Error starting HTTP server");
    }
}

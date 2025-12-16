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

// --- MANIPULADOR DE DOWNLOAD DE ARQUIVOS (CORRIGIDO) ---
static esp_err_t file_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Download request: %s", req->uri);

    // 1. FORÇAR O FECHAMENTO DA CONEXÃO AO FINAL
    // Isso impede que o socket fique preso (Keep-Alive) após o download
    httpd_resp_set_hdr(req, "Connection", "close");

    char filepath[FILE_PATH_MAX];
    snprintf(filepath, sizeof(filepath), MOUNT_POINT"%s", req->uri);

    FILE *file = fopen(filepath, "r");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");

    // Extrai nome do arquivo
    const char *filename = strrchr(req->uri, '/');
    if (filename == NULL) filename = req->uri;
    else filename++;

    static char filename_buf[MAX_FILENAME_LEN];
    strncpy(filename_buf, filename, MAX_FILENAME_LEN - 1);
    filename_buf[MAX_FILENAME_LEN - 1] = '\0';

    char disposition[256];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename_buf);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    // Buffer de 1024 bytes (Equilíbrio entre velocidade e memória)
    char *chunk = malloc(1024);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "Memory allocation failed");
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory error");
        return ESP_FAIL;
    }

    size_t chunksize;
    do {
        chunksize = fread(chunk, 1, 1024, file);
        if (chunksize > 0) {
            // Tenta enviar. Se der erro (como socket fechado), sai do loop.
            esp_err_t err = httpd_resp_send_chunk(req, chunk, chunksize);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Send failed/aborted (%d). Closing file.", err);
                fclose(file);
                free(chunk);
                return ESP_FAIL; // Retorna erro para fechar o socket imediatamente
            }
        }
        
        // Delay crítico para evitar o Erro 11 (Buffer Full)
        // Dá tempo para o ESP32 esvaziar o buffer TCP antes de ler mais do SD
        vTaskDelay(pdMS_TO_TICKS(20)); 

    } while (chunksize > 0);

    fclose(file);
    free(chunk);
    
    // Finaliza envio
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

// --- PÁGINA HTML (ESTILO MANTIDO) ---
static const char *HTML_HEADER = "<!DOCTYPE html>"
"<html lang=\"pt-BR\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
"<title>CO₂ Level Monitor</title><style>"
"body { font-family: Arial, sans-serif; background-color: #f0f0f0; margin: 0; padding: 0; }"
"header { background-color: #2196F3; color: white; padding: 20px; text-align: center; }"
"main { padding: 20px; }"
"table { width: 100%; border-collapse: collapse; background: white; }"
"th, td { padding: 12px; text-align: left; border-bottom: 1px solid #ddd; }"
"tr:hover { background-color: #f5f5f5; }"
"a { text-decoration: none; }"
".btn { background-color: #f44336; color: white; padding: 8px 16px; border: none; border-radius: 4px; cursor: pointer; }"
".btn-download { background-color: #4CAF50; color: white; padding: 8px 16px; border: none; border-radius: 4px; cursor: pointer; }"
"</style></head><body><header><h1>CO₂ Monitor</h1></header><main>";
static const char *HTML_FOOTER = "</main></body></html>";

static esp_err_t file_list_handler(httpd_req_t *req) {
    // Força fechar conexão da lista também, para liberar socket rápido
    httpd_resp_set_hdr(req, "Connection", "close");

    struct dirent *entry;
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open directory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HTML_HEADER);

    char date_str[11], time_str[9];
    get_current_date_time(date_str, sizeof(date_str), time_str, sizeof(time_str));
    char datetime_html[128];
    snprintf(datetime_html, sizeof(datetime_html), "<p><strong>Data/Hora:</strong> %s %s</p>", date_str, time_str);
    httpd_resp_sendstr_chunk(req, datetime_html);

    httpd_resp_sendstr_chunk(req, "<table><tr><th>Arquivo</th><th>Ações</th></tr>");

    static char line[512];
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char filename[MAX_FILENAME_LEN];
            strncpy(filename, entry->d_name, MAX_FILENAME_LEN - 1);
            filename[MAX_FILENAME_LEN - 1] = '\0';
            
            // Adicionado target="_blank" para download tentar nova aba e não travar a atual
            snprintf(line, sizeof(line),
                "<tr><td><a href=\"/%s\">%s</a></td>"
                "<td><a href=\"/%s\" target=\"_blank\"><button class=\"btn-download\">Download</button></a> "
                "<form method=\"GET\" action=\"/delete/%s\" onsubmit=\"return confirm('Excluir?');\" style=\"display:inline;\">"
                "<button type=\"submit\" class=\"btn\">Excluir</button></form></td></tr>",
                filename, filename, filename, filename);
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
void start_http_server(httpd_handle_t *server_handle_ptr) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // 1. Pilha grande para evitar Stack Overflow
    config.stack_size = 10240; 
    
    // 2. Prioridade um pouco acima do IDLE
    config.task_priority = tskIDLE_PRIORITY + 5;
    
    // 3. LIMPEZA DE CONEXÕES VELHAS (Fundamental)
    // Se o 5º usuário tentar entrar, derruba o mais antigo.
    config.lru_purge_enable = true; 
    
    // 4. TIMEOUTS MAIORES
    // O erro 11 significa "ocupado". Aumentamos o timeout para o servidor
    // ter paciência de esperar o buffer esvaziar antes de derrubar a conexão.
    config.send_wait_timeout = 20; // Padrão é 5s. Aumentado para 20s.
    config.recv_wait_timeout = 20; 

    config.max_open_sockets = 4;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server (Stack: %d, LRU: On)", config.stack_size);

    if (httpd_start(&server, &config) == ESP_OK) {
        *server_handle_ptr = server;
        
        httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler };
        httpd_register_uri_handler(server, &favicon);

        httpd_uri_t file_list = { .uri = "/", .method = HTTP_GET, .handler = file_list_handler };
        httpd_register_uri_handler(server, &file_list);

        httpd_uri_t file_del = { .uri = "/delete/*", .method = HTTP_GET, .handler = file_delete_handler };
        httpd_register_uri_handler(server, &file_del);

        httpd_uri_t file_dl = { .uri = "/*", .method = HTTP_GET, .handler = file_get_handler };
        httpd_register_uri_handler(server, &file_dl);

        ESP_LOGI(TAG, "HTTP server started");
    } else {
        ESP_LOGE(TAG, "Error starting HTTP server");
        *server_handle_ptr = NULL;
    }
}

#include <string.h>
#include <sys/dirent.h>
#include "http_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "sd_card.h"
#include "rtc.h"
#include "freertos/semphr.h"   // Necessário
#include "co2_sensor_task.h"   // Para get_quick_sensor_data

static const char *TAG = "HTTP_SERVER";
extern SemaphoreHandle_t xSensorMutex; // Pega o Mutex criado no main.c

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

static const char *HTML_HEADER = "<!DOCTYPE html>"
"<html lang=\"pt-BR\"><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
"<title>Monitor Ambiental</title><style>"
"body { font-family: 'Segoe UI', Arial, sans-serif; background-color: #e0e0e0; margin: 0; padding: 0; }"
"header { background-color: #00695c; color: white; padding: 15px; text-align: center; box-shadow: 0 2px 5px rgba(0,0,0,0.2); }"
"main { padding: 15px; max-width: 800px; margin: 0 auto; }"
".card { background: white; padding: 20px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); margin-bottom: 20px; }"
".data-box { display: flex; justify-content: space-around; flex-wrap: wrap; text-align: center; }"
".metric { margin: 10px; }"
".metric h3 { margin: 0; color: #555; font-size: 0.9em; }"
".metric p { margin: 5px 0 0; font-size: 1.5em; font-weight: bold; color: #00695c; }"
"table { width: 100%; border-collapse: collapse; margin-top: 10px; }"
"th, td { padding: 12px; text-align: left; border-bottom: 1px solid #ddd; }"
"tr:hover { background-color: #f9f9f9; }"
"a { text-decoration: none; }"
".btn { padding: 8px 12px; border: none; border-radius: 4px; cursor: pointer; font-size: 0.9em; transition: 0.2s; }"
".btn-dl { background-color: #4CAF50; color: white; }"
".btn-del { background-color: #F44336; color: white; }"
".btn-all { background-color: #2196F3; color: white; width: 100%; padding: 12px; font-size: 1.1em; margin-bottom: 15px; }"
".btn:hover { opacity: 0.9; }"
".status-busy { color: #F44336; font-style: italic; }"
"</style>"
"<script>"
"function downloadAll() {"
"  var links = document.querySelectorAll('.dl-link');"
"  if(links.length == 0) { alert('Nenhum arquivo para baixar!'); return; }"
"  if(!confirm('Isso iniciará o download de ' + links.length + ' arquivos. Continuar?')) return;"
"  var delay = 0;"
"  links.forEach(function(link) {"
"    setTimeout(function() { window.open(link.href, '_blank'); }, delay);"
"    delay += 1500;" // 1.5s de intervalo para proteger o servidor
"  });"
"}"
"</script>"
"</head><body><header><h1>Monitor CO₂ Inferior</h1></header><main>";

static const char *HTML_FOOTER = "</main></body></html>";

static esp_err_t file_list_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Connection", "close");

    // --- 1. TENTATIVA DE LEITURA (BAIXA PRIORIDADE) ---
    int co2_now = 0;
    float temp_now = 0, hum_now = 0;
    bool read_success = false;
    bool sensor_busy = false;

    // Tenta pegar o Mutex SEM ESPERAR (xBlockTime = 0)
    // Se o agendador estiver medindo (mutex preso), isso falha imediatamente (pdFALSE)
    if (xSemaphoreTake(xSensorMutex, 0) == pdTRUE) {
        // Conseguiu o sensor! Faz a leitura rápida
        read_success = get_quick_sensor_data(&co2_now, &temp_now, &hum_now);
        xSemaphoreGive(xSensorMutex); // Devolve rápido
    } else {
        // Falhou (Sensor ocupado pela medição principal)
        sensor_busy = true;
        ESP_LOGW(TAG, "Ignorando leitura web: Sensor em uso pela medição principal.");
    }

    // --- 2. MONTAGEM DA PÁGINA ---
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HTML_HEADER);

    // DATA E HORA ATUAIS
    char date_str[11], time_str[9];
    get_current_date_time(date_str, sizeof(date_str), time_str, sizeof(time_str));
    char datetime_html[128];
    snprintf(datetime_html, sizeof(datetime_html), "<p><strong>Data/Hora:</strong> %s %s</p>", date_str, time_str);
    httpd_resp_sendstr_chunk(req, datetime_html);
    // -------------------------------------

    // CARD DE DADOS TEMPO REAL
    char sensor_html[512];
    if (read_success) {
        snprintf(sensor_html, sizeof(sensor_html), 
            "<div class='card'><h2>Leitura Instantânea</h2>"
            "<div class='data-box'>"
            "<div class='metric'><h3>CO₂</h3><p>%d ppm</p></div>"
            "<div class='metric'><h3>Temp</h3><p>%.1f °C</p></div>"
            "<div class='metric'><h3>Umid</h3><p>%.1f %%</p></div>"
            "</div></div>", 
            co2_now, temp_now, hum_now);
    } else if (sensor_busy) {
        snprintf(sensor_html, sizeof(sensor_html), 
            "<div class='card'><h2>Leitura Instantânea</h2>"
            "<p class='status-busy'>⚠️ Medição Oficial em andamento. Visualização ignorada.</p></div>");
    } else {
        snprintf(sensor_html, sizeof(sensor_html), 
            "<div class='card'><h2>Leitura Instantânea</h2>"
            "<p class='status-busy'>Erro ou Aquecimento do Sensor.</p></div>");
    }
    httpd_resp_sendstr_chunk(req, sensor_html);

    // CARD DE ARQUIVOS
    httpd_resp_sendstr_chunk(req, "<div class='card'><h2>Histórico Diário</h2>");
    
    // Botão de Download em Massa
    httpd_resp_sendstr_chunk(req, "<button class='btn btn-all' onclick='downloadAll()'>📥 Baixar Todos os Arquivos</button>");
    
    httpd_resp_sendstr_chunk(req, "<table><tr><th>Data</th><th>Ações</th></tr>");

    // Lista arquivos do SD
    struct dirent *entry;
    DIR *dir = opendir(MOUNT_POINT);
    if (dir) {
        static char line[512];
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                char filename[128];
                strncpy(filename, entry->d_name, 127);
                filename[127] = '\0';
                
                // Adicionamos a classe 'dl-link' para o JS encontrar
                snprintf(line, sizeof(line),
                    "<tr><td>%s</td>"
                    "<td>"
                    "<a href=\"/%s\" target=\"_blank\" class=\"dl-link\"><button class=\"btn btn-dl\">Baixar</button></a> "
                    "<form method=\"GET\" action=\"/delete/%s\" onsubmit=\"return confirm('Excluir %s?');\" style=\"display:inline;\">"
                    "<button type=\"submit\" class=\"btn btn-del\">Excluir</button></form>"
                    "</td></tr>",
                    filename, filename, filename, filename);
                httpd_resp_sendstr_chunk(req, line);
            }
        }
        closedir(dir);
    } else {
        httpd_resp_sendstr_chunk(req, "<tr><td colspan='2'>Erro ao ler cartão SD</td></tr>");
    }
    
    httpd_resp_sendstr_chunk(req, "</table></div>");
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

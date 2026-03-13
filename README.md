# 🌲 Monitoramento Microclimático e de $CO_2$ com ESP32

Este repositório contém o código-fonte (*firmware*) de um sistema embarcado autônomo desenvolvido para o **Laboratório de Computação Natural (LCN)** e o **Centro de Inovação e Pesquisa e Extensão em Computação (CIPEC)** da Universidade Estadual do Sudoeste da Bahia (UESB).

O projeto atua como a infraestrutura tecnológica para a coleta de dados de uma pesquisa de Engenharia Florestal, realizando o monitoramento contínuo do fluxo de Dióxido de Carbono ($CO_2$), temperatura e umidade nos estratos da Floresta Nacional de Contendas do Sincorá.

---

## 🚀 Funcionalidades Principais

* **Aquisição Científica Cronometrada:** Leituras automáticas de $CO_2$ (sensor MH-Z16) e clima (DHT22) cravadas nos minutos `00` e `30` de cada hora, controladas por um Relógio de Tempo Real (RTC DS1302).
* **Processamento Dual-Core (FreeRTOS):** O sistema divide as cargas de trabalho. O **Core 0** gerencia a comunicação com os sensores (UART/SPI) e gravações no SD, enquanto o **Core 1** hospeda exclusivamente o servidor de rede Wi-Fi.
* **Segurança de Concorrência (Mutex):** Implementação de um *Mutex* (`xSensorMutex`) para garantir exclusão mútua. Impede que acessos simultâneos via página web corrompam as leituras agendadas no cartão de memória (*Race Conditions*).
* **Servidor HTTP Embarcado (Dashboard):** Gera uma rede Wi-Fi local (*SoftAP*). Os pesquisadores podem conectar seus smartphones na floresta para visualizar dados em tempo real e fazer o download em lote (via JavaScript) dos arquivos.
* **Consolidação de Dados em CSV:** Em vez de gerar arquivos fragmentados, o sistema usa o modo *append* para criar um único arquivo diário, inserindo algoritmicamente colunas cruciais para a pesquisa científica, como `Estrato` e `Turno_Medicao`.
* **Gerenciamento Energético Adaptado:** O firmware inibe intencionalmente os modos *Sleep* e força a transmissão do Wi-Fi na potência máxima (`esp_wifi_set_max_tx_power(78)`) para gerar um consumo basal que impede o desligamento automático dos *power banks* comerciais (burlando a restrição do BMS).

---

## 🛠️ Arquitetura de Hardware (Pinagem)

O circuito foi otimizado para operação robusta. Abaixo está o mapa de conexões GPIO para o microcontrolador ESP32:

| Componente | Pino ESP32 | Protocolo / Função |
| --- | --- | --- |
| **Sensor DHT22** | `GPIO 4` | Leitura digital (1-Wire) |
| **Sensor MH-Z16** (TX) | `GPIO 16` (RX2) | UART1 RX |
| **Sensor MH-Z16** (RX) | `GPIO 17` (TX2) | UART1 TX |
| **Módulo SD HW-203** (MISO) | `GPIO 19` | SPI MISO |
| **Módulo SD HW-203** (MOSI) | `GPIO 21` | SPI MOSI |
| **Módulo SD HW-203** (CLK) | `GPIO 18` | SPI Clock |
| **Módulo SD HW-203** (CS) | `GPIO 5` | SPI Chip Select |
| **RTC DS1302** (CLK) | `GPIO 27` | Clock |
| **RTC DS1302** (DAT/IO) | `GPIO 26` | Data / I/O |
| **RTC DS1302** (RST/CE) | `GPIO 25` | Reset / Chip Enable |

> **Nota de Alimentação:** O sistema deve ser alimentado por uma fonte estável de 5V (recomenda-se *power bank* acoplado a painel solar). A inicialização possui um *delay* via software para mitigar picos de *Inrush Current* provenientes do acionamento da antena de radiofrequência.

---

## 📂 Estrutura dos Dados (Saída CSV)

Os arquivos gerados no Cartão MicroSD seguem a nomenclatura `YYYY-MM-DD-Estrato.csv` (ex: `2026-01-20-Medio.csv`) e possuem a seguinte estruturação de colunas:

```csv
Date;Time;CO2_PPM;Temperatura;Umidade;Estrato;Turno_Medicao
2026-01-20;07:01:03;415;24.1;85.8;Medio;Manha
2026-01-20;11:31:03;380;36.2;52.4;Medio;Zenite
2026-01-20;16:31:03;400;31.0;63.5;Medio;Entardecer

```

---

## ⚙️ Pré-requisitos e Instalação

Este projeto foi desenvolvido utilizando o **ESP-IDF** (Espressif IoT Development Framework).

1. Instale o [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/).
2. Clone este repositório:
```bash
git clone https://github.com/jonathasporto/co2-meter-using-ESP32.git
cd co2-meter-using-ESP32

```


3. Configure o ambiente (se necessário) e defina o alvo para o ESP32:
```bash
idf.py set-target esp32

```


4. Compile e faça o *flash* para o microcontrolador:
```bash
idf.py build flash monitor

```



---

## 📱 Guia de Uso Operacional (Em Campo)

Para extrair os dados na floresta sem necessidade de internet:

1. Aproxime-se da estação de monitoramento. O ESP32 estará roteando uma rede Wi-Fi.
2. Conecte seu dispositivo móvel à rede:
* **SSID:** `ESP32_CO2_MEDIO` *(O nome varia de acordo com o estrato compilado no main.c)*
* **Senha:** `12345678`


3. Abra o navegador e acesse o IP estático do Servidor HTTP:
* **URL:** `http://192.168.4.1`


4. O **Dashboard** será carregado exibindo as leituras instantâneas do momento e a lista de arquivos diários.
5. Clique em **"Baixar Todos os Arquivos"** para fazer o download em lote de todos os relatórios CSV gerados desde a última extração.

---

## 👨‍💻 Autores e Agradecimentos

* **Desenvolvedor Principal:** Jonathas Porto - *Engenharia de Software & Hardware (BCC / UESB)*
* **Orientação Técnica:** Prof. Hélio Lopes dos Santos
* **Pesquisador (Eng. Florestal):** Leandro

Pesquisa financiada e apoiada pela Universidade Estadual do Sudoeste da Bahia (UESB) através do **Laboratório de Computação Natural (LCN)**.

---

*Em conformidade com as Boas Práticas Científicas, este repositório atua como registro e versionamento metodológico para garantir a reprodutibilidade tecnológica do projeto.*

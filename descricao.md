# Relatório Técnico Detalhado: Sistema Autônomo para Monitoramento de Fluxo de Carbono

## 1. Visão Geral e Finalidade do Projeto

### 1.1. Objetivo Científico
O presente sistema foi desenvolvido como um data logger autônomo e de baixo consumo para a coleta de dados ambientais em campo, com o objetivo principal de monitorar o fluxo de dióxido de carbono (CO₂). O projeto visa dar suporte a estudos científicos na **Floresta Nacional (FLONA) de Contendas do Sincorá, Bahia**. Além do CO₂, o sistema mede e registra dados contextuais de temperatura e umidade relativa do ar.

### 1.2. Requisitos Técnicos
O sistema foi projetado para atender a requisitos rigorosos de operação em campo:
* **Autonomia Energética:** Operação por longos períodos (meses) alimentado por um conjunto de baterias 18650.
* **Operação Autônoma:** Coleta de dados em horários pré-agendados sem qualquer intervenção humana.
* **Robustez:** Resiliência a falhas de energia, com manutenção da integridade temporal dos dados através de um Relógio de Tempo Real (RTC) com bateria de backup.
* **Armazenamento Local:** Gravação de todos os dados em um cartão Micro SD em formato universal (`.csv`).
* **Acesso Sob Demanda:** Disponibilização dos dados para coleta em campo através de uma interface Wi-Fi local, acessível por um botão, eliminando a necessidade de remover o cartão SD.

## 2. Arquitetura de Hardware

### 2.1. Lista de Componentes

| Categoria | Componente | Especificação / Modelo | Função no Sistema |
| :--- | :--- | :--- | :--- |
| **Módulos Principais**| Microcontrolador | ESP32 DevKitC | Cérebro do sistema, responsável pelo controle, processamento e modos de baixo consumo. |
| | Sensor de CO₂ | MH-Z14A | Medição da concentração de CO₂ (400-5000 ppm) via sensor infravermelho não dispersivo (NDIR). |
| | Sensor Temp/Umid. | DHT22 / AM2302 | Medição da temperatura e umidade relativa do ar. |
| | Relógio (RTC) | Módulo DS1302 com Bateria CR2032 | Manutenção da data e hora corretas de forma independente, mesmo sem a alimentação principal. |
| | Armazenamento | Módulo Leitor de Cartão Micro SD (HW-203) | Armazenamento dos dados coletados em formato `.csv`. |
| **Alimentação** | Regulador de Tensão| Módulo LM2596 | Converte a tensão das baterias (~7.4V) para 5V estáveis. |
| | Fonte de Energia | 2x Baterias 18650 | Alimentação principal do sistema. |
| | | Suporte para 2 Baterias 18650 | Conexão física das baterias. |
| **Atuadores e Interface** | Ventoinha (Fan) | Brushless 5V DC | Purga o ar da caixa de medição antes das coletas para garantir uma amostra fresca e representativa. |
| | Botão | Módulo Push Button | Ativa o modo de acesso Wi-Fi para coleta de dados. |
| **Componentes de Suporte**| Transistor | NPN de Uso Geral (BC548/2N2222) | Atua como chave eletrônica para que o ESP32 possa ligar/desligar o fan de 5V. |
| | Resistores | 1x 1kΩ, 2x 10kΩ, 1x 4.7kΩ | Funções de pull-up para estabilizar a comunicação com os sensores (DHT22, SD Card) e limitar a corrente para o transistor. |

### 2.2. Diagrama de Conexão
As conexões seguem um padrão que otimiza o uso dos periféricos do ESP32 (SPI, UART) e pinos de uso geral (GPIO), garantindo a estabilidade do sistema com o uso de resistores de pull-up.

**(A tabela de conexão final e completa que já foi fornecida anteriormente seria inserida aqui, detalhando cada pino e componente.)**

## 3. Arquitetura de Software e Lógica de Funcionamento Detalhada

O firmware foi desenvolvido utilizando o framework **ESP-IDF**, que se baseia no sistema operacional de tempo real **FreeRTOS**. A arquitetura principal do software é uma **máquina de estados** controlada pelo modo de **Deep Sleep**, garantindo o mínimo consumo de energia.

### 3.1. Módulos de Software (Análise por Arquivo)

* **`main.c`**: Este é o "cérebro" do sistema. A função `app_main()` é executada a cada despertar do ESP32. Sua primeira ação é verificar a causa do despertar (`esp_sleep_get_wakeup_cause()`) para decidir qual modo de operação executar. Este arquivo contém a lógica principal da máquina de estados e a função `goToDeepSleep()`, que calcula o tempo para a próxima medição e coloca o dispositivo para "hibernar".

* **`co2_sensor_task.c`**: Contém a função `perform_single_measurement()`, que é o coração da rotina de coleta de dados. Esta função não é uma tarefa em loop, mas uma sequência de ações executada de uma só vez quando chamada pelo `main.c`.

* **`rtc.c`**: Este módulo é o "guardião do tempo". Ele abstrai a comunicação com o chip DS1302. Sua função mais crítica, `initialize_rtc()`, acerta o relógio do hardware com a hora da compilação (apenas se a bateria do RTC falhar) e, o mais importante, sincroniza o relógio interno do sistema do ESP32 com a hora lida do hardware, garantindo consistência em todo o programa.

* **`sd_card.c`**: Gerencia toda a interação com o cartão SD. Inicializa o barramento SPI, monta o sistema de arquivos (VFAT), e contém as funções para criar novos arquivos `.csv` e para escrever os dados formatados.

* **`http_server.c`**: Contém a lógica para o modo de acesso aos dados. A função `start_http_server()` inicializa um servidor web com manipuladores de URL para listar os arquivos do cartão SD, permitir o download e a exclusão segura.

### 3.2. Ciclos de Operação Detalhados

O sistema opera em dois modos principais, mutuamente exclusivos:

#### 3.2.1. Ciclo de Medição Autônoma (Despertar por Timer)
Este é o modo de operação padrão do dispositivo.
1.  **Despertar:** O ESP32 acorda de um Deep Sleep no horário pré-calculado.
2.  **Sincronização:** A função `initialize_rtc()` é chamada, lendo a hora do DS1302 e sincronizando o relógio do sistema.
3.  **Verificação de Agenda:** O código verifica se a hora atual corresponde a um dos horários de medição definidos (a cada 30 minutos, dentro dos intervalos de 07:00–09:00, 11:00–13:00 e 16:00–18:00).
4.  **Rotina de Medição:** Se a hora for correspondente, a função `perform_single_measurement()` é chamada:
    a.  O **cartão SD é inicializado**.
    b.  A **ventoinha (fan) é ativada por 9 segundos** para purgar o ar estagnado.
    c.  A ventoinha é desligada e o sistema aguarda 1 segundo.
    d.  É feita **uma leitura do sensor DHT22** (temperatura e umidade).
    e.  É iniciada uma **"rajada" de medições do sensor MH-Z14A**. O sistema calcula a **mediana** desses valores, obtendo um resultado único e estável.
    f.  Os dados finais (Data, Hora, CO₂ Mediana, Temperatura, Umidade) são formatados e **gravados em uma nova linha no arquivo `.csv`** no cartão SD.
    g.  O arquivo no cartão SD é fechado para garantir a integridade dos dados.
5.  **Hibernar:** A função `goToDeepSleep()` é chamada. Ela calcula quantos segundos faltam até a próxima medição de 30 minutos, configura o timer de despertar e coloca o ESP32 em Deep Sleep.

#### 3.2.2. Ciclo de Acesso aos Dados (Despertar por Botão)
Este modo é ativado manualmente pelo usuário para a coleta dos dados.
1.  **Despertar:** O usuário pressiona o botão, e o ESP32 acorda do Deep Sleep.
2.  **Detecção da Causa:** A função `app_main` detecta que a causa do despertar foi o pino externo.
3.  **Ativação do Wi-Fi e Servidor:** O código ignora a rotina de medição e chama as funções `wifi_init_softap()` e `start_http_server()`, criando uma rede Wi-Fi local ("ESP32_CO2") e ativando um servidor web.
4.  **Janela de Atividade:** O sistema permanece acordado por **15 minutos**. Durante este tempo, um pesquisador pode se conectar à rede, acessar o endereço `http://192.168.4.1` e usar a interface para listar, baixar e excluir os arquivos de dados.
5.  **Hibernar:** Após os 15 minutos, o Wi-Fi é desligado e a função `goToDeepSleep()` é chamada para retornar o dispositivo ao seu ciclo normal de medição, preservando a bateria.

### 3.3. Lógica de Tempo Robusta
O sistema foi projetado para ser resiliente a falhas de energia.
* **Operação Normal:** O tempo é mantido pelo módulo DS1302 e sua bateria. A cada despertar, o ESP32 sincroniza seu relógio interno com o do DS1302.
* **Falha da Bateria do RTC:** Se a bateria do DS1302 falhar, o código detecta que o relógio está "parado". Nesta situação, e somente nesta, ele usa a hora da compilação do software para "dar a partida" no relógio novamente.
* **Verificação de Sanidade:** Antes de sincronizar, o código verifica se a data lida do RTC é plausível (ex: ano entre 2024-2098) para evitar que dados corrompidos contaminem os registros de tempo.

### 3.4. Modo de Teste
Para facilitar a depuração, uma chave de compilação (`#define MODO_DE_TESTE`) foi implementada no `main.c`. Quando ativada, ela altera o comportamento do Deep Sleep para acordar a cada 60 segundos e realizar uma medição, independentemente do horário, permitindo uma verificação rápida de todo o ciclo funcional do sistema.

## 4. Conclusão
O sistema desenvolvido é um data logger autônomo, de baixo consumo e robusto, com capacidade de acesso Wi-Fi sob demanda. A arquitetura de software baseada em Deep Sleep e a metodologia de medição por mediana garantem a autonomia energética e a qualidade dos dados, respectivamente, tornando o dispositivo uma ferramenta eficaz para pesquisas científicas de campo de longa duração.
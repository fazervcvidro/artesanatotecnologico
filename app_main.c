/* Get recv router csi

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, software is
   distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
   either express or implied.
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h" // Include para FreeRTOS Queue

#include "nvs_flash.h"

#include "esp_mac.h"
#include "rom/ets_sys.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_now.h" // Mantido do original, mas não usado neste exemplo de envio via socket

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h" // Include para funções de socket (UDP/TCP)
#include "ping/ping_sock.h" // Mantido do original

#include "protocol_examples_common.h" // Para example_connect()
#include "errno.h" // Include para errno
#include <unistd.h> // Include para close()

#include "esp_sleep.h" // Inclui suporte ao deep sleep

// ===============================================================================
// Configurações para Envio de Dados (UDP)
// ===============================================================================
// Endereço IP do seu servidor (substitua pelo IP real da sua máquina)
#define SERVER_IP "192.168.100.158"
// Porta UDP no seu servidor (substitua pela porta real onde seu programa receptor escuta)
#define SERVER_PORT 12345

// Tamanho máximo estimado para a string de dados CSI formatada.
// Ajuste se os logs indicarem que a fila está cheia ou se os dados forem truncados.
#define MAX_CSI_DATA_LEN 2048

// Handle para a fila que armazenará os dados CSI formatados (strings)
static QueueHandle_t csi_data_queue;

// Handle do socket UDP
static int udp_sock = -1;

// ===============================================================================


#define CONFIG_SEND_FREQUENCY       100
#if CONFIG_IDF_TARGET_ESP32C5
    #define CSI_FORCE_LLTF                      1
#endif
#define CONFIG_FORCE_GAIN                   1
// Defina CONFIG_GAIN_CONTROL no menuconfig se quiser usar o controle de ganho
// #define CONFIG_GAIN_CONTROL 1

static const char *TAG = "csi_recv_router";
typedef struct
{
    unsigned : 32; /**< reserved */
    unsigned : 32; /**< reserved */
    unsigned : 32; /**< reserved */
    unsigned : 32; /**< reserved */
    unsigned : 32; /**< reserved */
#if CONFIG_IDF_TARGET_ESP32S2
    unsigned : 32; /**< reserved */
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C5 ||CONFIG_IDF_TARGET_ESP32C6
    unsigned : 16; /**< reserved */
    unsigned fft_gain : 8;
    unsigned agc_gain : 8;
    unsigned : 32; /**< reserved */
#endif
    unsigned : 32; /**< reserved */
#if CONFIG_IDF_TARGET_ESP32S2
     signed : 8;  /**< reserved */
    unsigned : 24; /**< reserved */
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6 // Adicionado C6 aqui
    unsigned : 32; /**< reserved */
    unsigned : 32; /**< reserved */
    unsigned : 32; /**< reserved */
#endif
    unsigned : 32; /**< reserved */
} wifi_pkt_rx_ctrl_phy_t;

#if CONFIG_FORCE_GAIN
    /**
     * @brief Enable/disable automatic fft gain control and set its value
     * @param[in] force_en true to disable automatic fft gain control
     * @param[in] force_value forced fft gain value
     */
    extern void phy_fft_scale_force(bool force_en, uint8_t force_value);

    /**
     * @brief Enable/disable automatic gain control and set its value
     * @param[in] force_en true to disable automatic gain control
     * @param[in] force_value forced gain value
     */
    extern void phy_force_rx_gain(int force_en, int force_value);
#endif


// Task para enviar dados CSI coletados via rede (UDP)
static void network_send_task(void *pvParameters)
{
    char csi_string[MAX_CSI_DATA_LEN];
    struct sockaddr_in server_addr;

    // Configurar o endereço do servidor
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT); // Converte a porta para network byte order
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr.s_addr); // Converte IP de string para estrutura

    // Criar o socket UDP
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "Erro ao criar socket UDP: errno %d", errno);
        vTaskDelete(NULL); // Não é possível continuar sem socket
    }
    ESP_LOGI(TAG, "Socket UDP criado");

    // Loop principal da task de envio
    while (1) {
        // Espera por dados na fila. Bloqueia indefinidamente (portMAX_DELAY) até que um item esteja disponível.
        if (xQueueReceive(csi_data_queue, csi_string, portMAX_DELAY) == pdTRUE) {
            // Dados recebidos da fila, enviar pela rede

            int len = strlen(csi_string);
            int err = sendto(udp_sock, csi_string, len, 0, (struct sockaddr *)&server_addr, sizeof(server_addr));

            if (err < 0) {
                // Loga o erro, mas continua, pois UDP não é orientado a conexão
                ESP_LOGE(TAG, "Erro no envio UDP: errno %d", errno);
                // Para UDP, geralmente não fechamos o socket aqui a menos que o erro seja crítico
            } else {
                 // Opcional: Log para confirmar envio
                 // ESP_LOGI(TAG, "Dados enviados (%d bytes) para %s:%d", err, SERVER_IP, SERVER_PORT);
            }
        }
    }

    // O código nunca deve chegar aqui em um loop infinito, mas por segurança:
    if (udp_sock != -1) {
        close(udp_sock);
    }
    vTaskDelete(NULL);
}


static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf) {
        // Use ESP_LOGW para avisos, menos crítico que ESP_LOGE
        ESP_LOGW(TAG, "CSI callback recebeu dados inválidos");
        return;
    }

    // Compara o MAC do pacote com o MAC do router (ctx)
    if (memcmp(info->mac, ctx, 6)) {
        return; // Ignora pacotes de outros MACs
    }

    // Use static para manter a contagem entre chamadas
    static int s_count = 0;

    // Desempacota as informações de controle de recepção e phy
    const wifi_pkt_rx_ctrl_t *rx_ctrl = &info->rx_ctrl;
    wifi_pkt_rx_ctrl_phy_t *phy_info = (wifi_pkt_rx_ctrl_phy_t *)info;

#if CONFIG_GAIN_CONTROL
    // Lógica de controle de ganho (mantida do código original)
    static uint16_t agc_gain_sum=0;
    static uint16_t fft_gain_sum=0;
    static uint8_t agc_gain_force_value=0;
    static uint8_t fft_gain_force_value=0;
    if (s_count<100) {
        agc_gain_sum += phy_info->agc_gain;
        fft_gain_sum += phy_info->fft_gain;
    }else if (s_count == 100) {
        agc_gain_force_value = agc_gain_sum/100;
        fft_gain_force_value = fft_gain_sum/100;
    #if CONFIG_FORCE_GAIN
        phy_fft_scale_force(1,fft_gain_force_value);
        phy_force_rx_gain(1,agc_gain_force_value);
    #endif
        ESP_LOGI(TAG,"fft_force %d, agc_force %d",fft_gain_force_value,agc_gain_force_value);
    }
#endif

    // --- Lógica de Formatação e Envio para a Fila ---

    // Use um buffer local para formatar a string antes de enviar para a fila
    // Garante que não escrevemos mais do que o tamanho alocado
    char temp_csi_string[MAX_CSI_DATA_LEN];
    int offset = 0; // Para rastrear a posição atual no buffer

    // Formata o cabeçalho CSI (similar ao seu ets_printf)
    // Use snprintf para segurança (evita buffer overflow)
#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6
    // Imprime o cabeçalho no console apenas na primeira vez para depuração
    if (s_count == 0) {
        ESP_LOGI(TAG, "================ CSI RECV ================");
        ets_printf("type,seq,mac,rssi,rate,noise_floor,fft_gain,agc_gain,channel,local_timestamp,sig_len,rx_state,len,first_word,data\n");
    }
    offset += snprintf(temp_csi_string + offset, MAX_CSI_DATA_LEN - offset,
                       "CSI_DATA,%d," MACSTR ",%d,%d,%d,%d,%d,%d,%d,%d,%d",
                       s_count, MAC2STR(info->mac), rx_ctrl->rssi, rx_ctrl->rate,
                       rx_ctrl->noise_floor, phy_info->fft_gain, phy_info->agc_gain, rx_ctrl->channel,
                       rx_ctrl->timestamp, rx_ctrl->sig_len, rx_ctrl->rx_state);
#else // Para ESP32, ESP32S2, ESP32S3, ESP32C3
    // Imprime o cabeçalho no console apenas na primeira vez para depuração
    if (s_count == 0) {
        ESP_LOGI(TAG, "================ CSI RECV ================");
        ets_printf("type,id,mac,rssi,rate,sig_mode,mcs,bandwidth,smoothing,not_sounding,aggregation,stbc,fec_coding,sgi,noise_floor,ampdu_cnt,channel,secondary_channel,local_timestamp,ant,sig_len,rx_state,len,first_word,data\n");
    }
    offset += snprintf(temp_csi_string + offset, MAX_CSI_DATA_LEN - offset,
                        "CSI_DATA,%d," MACSTR ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                        s_count, MAC2STR(info->mac), rx_ctrl->rssi, rx_ctrl->rate, rx_ctrl->sig_mode,
                        rx_ctrl->mcs, rx_ctrl->cwb, rx_ctrl->smoothing, rx_ctrl->not_sounding,
                        rx_ctrl->aggregation, rx_ctrl->stbc, rx_ctrl->fec_coding, rx_ctrl->sgi,
                        rx_ctrl->noise_floor, rx_ctrl->ampdu_cnt, rx_ctrl->channel, rx_ctrl->secondary_channel,
                        rx_ctrl->timestamp, rx_ctrl->ant, rx_ctrl->sig_len, rx_ctrl->rx_state);
#endif

    // Formata os dados CSI (a parte info->buf)
#if CONFIG_IDF_TARGET_ESP32C5 && CSI_FORCE_LLTF
    offset += snprintf(temp_csi_string + offset, MAX_CSI_DATA_LEN - offset,
                       ",%d,%d,\"[%d", (info->len - 2) / 2, info->first_word_invalid,
                       (int16_t)(((int16_t)info->buf[1]) << 12) >> 4 | (uint8_t)info->buf[0]); // Primeiro valor
    for (int i = 2; i < (info->len - 2); i += 2) {
        // Certifica-se de que há espaço suficiente no buffer antes de adicionar mais dados
        if (offset + 15 > MAX_CSI_DATA_LEN) break; // Aproximação de espaço necessário para ",%d"
        offset += snprintf(temp_csi_string + offset, MAX_CSI_DATA_LEN - offset, ",%d",
                           (int16_t)(((int16_t)info->buf[i + 1]) << 12) >> 4 | (uint8_t)info->buf[i]);
    }
#else // Para ESP32, ESP32S2, ESP32S3, ESP32C3, ESP32C6
    offset += snprintf(temp_csi_string + offset, MAX_CSI_DATA_LEN - offset,
                       ",%d,%d,\"[%d", info->len, info->first_word_invalid, info->buf[0]); // Primeiro valor
    for (int i = 1; i < info->len; i++) {
         // Certifica-se de que há espaço suficiente no buffer antes de adicionar mais dados
        if (offset + 15 > MAX_CSI_DATA_LEN) break; // Aproximação de espaço necessário para ",%d"
        offset += snprintf(temp_csi_string + offset, MAX_CSI_DATA_LEN - offset, ",%d", info->buf[i]);
    }
#endif
    // Finaliza a string com "]" e quebra de linha
    offset += snprintf(temp_csi_string + offset, MAX_CSI_DATA_LEN - offset, "]\"\n");

    // Certifica-se de que a string está null-terminated no final do que foi escrito
    temp_csi_string[MAX_CSI_DATA_LEN - 1] = '\0';


    // **Envie a string formatada para a fila**
    // Usamos xQueueSend com timeout 0 para não bloquear o callback.
    // Se a fila estiver cheia, o dado será descartado.
    BaseType_t ret = xQueueSend(csi_data_queue, temp_csi_string, 0);

    if (ret != pdTRUE) {
        // A fila está cheia. Isso significa que a task de envio não está acompanhando
        // a taxa de chegada dos dados CSI. Logamos um aviso.
        // ESP_LOGW(TAG, "Fila CSI cheia, dado descartado (count=%d)", s_count);
        // Evitamos logar em todas as perdas para não sobrecarregar o sistema de log
    } else {
        // Dado enviado com sucesso para a fila
        // ESP_LOGD(TAG, "Dado CSI enviado para fila (count=%d)", s_count);
    }

    s_count++;
}

static void wifi_csi_init()
{
    /**
     * @brief In order to ensure the compatibility of routers, only LLTF sub-carriers are selected.
     */
#if CONFIG_IDF_TARGET_ESP32C5
    wifi_csi_config_t csi_config = {
        .enable                         = true,
        .acquire_csi_legacy             = true,
        .acquire_csi_force_lltf         = CSI_FORCE_LLTF,
        .acquire_csi_ht20               = true,
        .acquire_csi_ht40               = true,
        .acquire_csi_vht                = false,
        .acquire_csi_su                 = false,
        .acquire_csi_mu                 = false,
        .acquire_csi_dcm                = false,
        .acquire_csi_beamformed         = false,
        .acquire_csi_he_stbc_mode       = 2,
        .val_scale_cfg                  = 0,
        .dump_ack_en                    = false,
        .reserved                       = false
    };
#elif CONFIG_IDF_TARGET_ESP32C6
    wifi_csi_config_t csi_config = {
        .enable                         = true,
        .acquire_csi_legacy             = true,
        .acquire_csi_ht20               = true,
        .acquire_csi_ht40               = true,
        .acquire_csi_su                 = false,
        .acquire_csi_mu                 = false,
        .acquire_csi_dcm                = false,
        .acquire_csi_beamformed         = false,
        .acquire_csi_he_stbc            = 2,
        .val_scale_cfg                  = false, // Verificar o tipo correto para C6 se necessário
        .dump_ack_en                    = false,
        .reserved                       = false
    };
#else // Para ESP32, ESP32S2, ESP32S3, ESP32C3
    wifi_csi_config_t csi_config = {
        .lltf_en            = true,
        .htltf_en           = false,
        .stbc_htltf2_en     = false,
        .ltf_merge_en       = true,
        .channel_filter_en  = true,
        .manu_scale         = true,
        .shift              = true,
    };
#endif
    static wifi_ap_record_t s_ap_info = {0};
    // Obtém as informações do ponto de acesso conectado para usar o BSSID como filtro
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&s_ap_info));
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    // Registra o callback CSI e passa o BSSID do AP como contexto (ctx)
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, s_ap_info.bssid));
    // Habilita a coleta de CSI
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));

    ESP_LOGI(TAG, "Coleta de CSI habilitada");
}

static esp_err_t wifi_ping_router_start()
{
    static esp_ping_handle_t ping_handle = NULL;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.count               = 0; // Pingar indefinidamente
    ping_config.interval_ms         = 1000 / CONFIG_SEND_FREQUENCY; // Intervalo entre pings
    ping_config.task_stack_size     = 3072; // Stack para a task de ping
    ping_config.data_size           = 1; // Tamanho mínimo do pacote ping

    esp_netif_ip_info_t local_ip;
    // Obtém o handle da interface WiFi STA e suas informações de IP
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &local_ip);
    ESP_LOGI(TAG, "got ip:" IPSTR ", gw: " IPSTR, IP2STR(&local_ip.ip), IP2STR(&local_ip.gw));

    // Define o gateway (roteador) como alvo do ping
    ping_config.target_addr.u_addr.ip4.addr = ip4_addr_get_u32(&local_ip.gw);
    ping_config.target_addr.type = ESP_IPADDR_TYPE_V4;

    esp_ping_callbacks_t cbs = { 0 }; // Não usamos callbacks de ping neste exemplo
    ESP_ERROR_CHECK(esp_ping_new_session(&ping_config, &cbs, &ping_handle));
    ESP_ERROR_CHECK(esp_ping_start(ping_handle));
    ESP_LOGI(TAG, "Ping para o roteador iniciado");

    return ESP_OK;
}

// =================== DEFINIÇÕES DE TEMPO ===================
#define DEEP_SLEEP_TIME_US (60 * 1000000) // 1 minuto em microssegundos
#define CSI_COLLECTION_TIME_MS (60 * 1000) // 1 minuto em milissegundos
#define UDP_LISTEN_TIMEOUT_SEC 10 // 10 segundos para aguardar comando 'start'
// ==========================================================

// =================== FUNÇÃO PARA AGUARDAR COMANDO UDP ===================
// Aguarda por um pacote UDP com a mensagem "start" por até UDP_LISTEN_TIMEOUT_SEC segundos
// Retorna 1 se recebeu o comando, 0 caso contrário
static int aguardar_comando_start(void) {
    int sockfd;
    struct sockaddr_in servaddr, cliaddr;
    char buffer[64];
    int ret = 0;

    // Cria socket UDP
    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        ESP_LOGE(TAG, "Erro ao criar socket UDP para comando start");
        return 0;
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(SERVER_PORT); // Usa a mesma porta do servidor

    // Faz bind na porta
    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        ESP_LOGE(TAG, "Erro no bind do socket UDP para comando start");
        close(sockfd);
        return 0;
    }

    // Configura timeout de recebimento
    struct timeval tv;
    tv.tv_sec = UDP_LISTEN_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    socklen_t len = sizeof(cliaddr);
    int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&cliaddr, &len);
    if (n > 0) {
        buffer[n] = '\0';
        if (strcmp(buffer, "start") == 0) {
            ret = 1;
            ESP_LOGI(TAG, "Comando 'start' recebido via UDP");
        } else {
            ESP_LOGI(TAG, "Comando UDP recebido, mas não é 'start': %s", buffer);
        }
    } else {
        ESP_LOGI(TAG, "Nenhum comando 'start' recebido em %d segundos", UDP_LISTEN_TIMEOUT_SEC);
    }
    close(sockfd);
    return ret;
}
// =======================================================================

// =================== MODIFICAÇÃO DA app_main ===================
void app_main()
{
    // Inicializa NVS (Non-Volatile Storage) - necessário para configurações WiFi
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    while (1) {
        ESP_LOGI(TAG, "Acordou do deep sleep. Iniciando ciclo...");

        // Conecta ao Wi-Fi
        ESP_ERROR_CHECK(example_connect());

        // Aguarda conexão WiFi e IP válido
        ESP_LOGI(TAG, "Aguardando conexão WiFi...");
        esp_netif_ip_info_t ip_info;
        int retries = 0;
        const int max_retries = 30;
        do {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
            retries++;
            if (retries > max_retries) {
                ESP_LOGE(TAG, "Falha ao obter IP após %d tentativas. Dormindo novamente.", max_retries);
                esp_deep_sleep(DEEP_SLEEP_TIME_US);
            }
        } while (ip_info.ip.addr == 0);
        ESP_LOGI(TAG, "WiFi conectado, IP: " IPSTR ", Gateway: " IPSTR, IP2STR(&ip_info.ip), IP2STR(&ip_info.gw));

        // Aguarda comando UDP 'start' por até 10 segundos
        if (!aguardar_comando_start()) {
            ESP_LOGI(TAG, "Comando 'start' não recebido. Indo para deep sleep.");
            esp_deep_sleep(DEEP_SLEEP_TIME_US);
        }

        // Se chegou aqui, recebeu o comando 'start'. Inicia coleta de CSI por 1 minuto
        ESP_LOGI(TAG, "Iniciando coleta de CSI por 1 minuto...");

        // Cria a fila para os dados CSI
        csi_data_queue = xQueueCreate(10, MAX_CSI_DATA_LEN);
        if (csi_data_queue == NULL) {
            ESP_LOGE(TAG, "Falha ao criar a fila CSI! Indo para deep sleep.");
            esp_deep_sleep(DEEP_SLEEP_TIME_US);
        }

        // Cria a task que enviará os dados CSI pela rede
        BaseType_t xReturned = xTaskCreate(network_send_task, "csi_sender", 4096, NULL, 5, NULL);
        if (xReturned != pdPASS) {
            ESP_LOGE(TAG, "Falha ao criar a task de envio de rede! Indo para deep sleep.");
            esp_deep_sleep(DEEP_SLEEP_TIME_US);
        }

        // Inicializa o subsistema de coleta de CSI
        wifi_csi_init();

        // Inicia o ping para o roteador (opcional, pode manter para manter link ativo)
        wifi_ping_router_start();

        // Aguarda 1 minuto coletando CSI
        vTaskDelay(pdMS_TO_TICKS(CSI_COLLECTION_TIME_MS));

        ESP_LOGI(TAG, "Fim do ciclo de coleta. Indo para deep sleep por 1 minuto.");
        // Encerra coleta de CSI e libera recursos se necessário
        esp_wifi_set_csi(false);
        // Fecha socket UDP se necessário (já é fechado na task)
        // Destroi fila
        vQueueDelete(csi_data_queue);
        // Dorme por 1 minuto
        esp_deep_sleep(DEEP_SLEEP_TIME_US);
    }
}
// =============================================================
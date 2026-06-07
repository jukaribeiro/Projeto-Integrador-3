extern "C" 
{
    #include "driver/i2c_master.h"
    #include "driver/gpio.h"
    #include <stdio.h>
    #include "esp_err.h"
    #include "esp_log.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"    
    #include "esp_lvgl_port.h"
    #include "lvgl.h"
    #include "displaySSD1306.h"
    
    // Bibliotecas do WiFi
    #include "esp_wifi.h"
    #include "esp_event.h"
    #include "esp_netif.h"
    #include "esp_http_server.h"
    #include "nvs_flash.h"
}

#include "cSMP3011.h"
#include "CBMP280.h"
#include "CGlobalResources.h"

#define LED_PIN               gpio_num_t::GPIO_NUM_16  
#define BOTAO_FINALIZAR_PIN   gpio_num_t::GPIO_NUM_0

// Constantes para detecção de pneu
#define PRESSAO_LIMIAR_CONEXAO_KPA     3.0f    // Detecta subida > 2 kPa
#define PRESSAO_LIMIAR_DESCONEXAO_KPA  1.0f    // Detecta volta < 1 kPa
#define TEMPO_DESCONEXAO_MS            500     // Meio segundo
#define MAX_PNEUS                      10

// Máquina de Estados Robusta
typedef enum {
    ESTADO_AMBIENTE,
    ESTADO_AGUARDANDO_PNEU,
    ESTADO_MEDINDO_PNEU
} estado_manometro_t;

/*
    PROTOTYPES
*/
void sensorSMP3011Task(void *pvParameters);
void sensorBMP280Task(void *pvParameters);
void statusLedTask(void *pvParameters);
void botoesTask(void *pvParameters);
void start_webserver(void);
float kpa_to_psi(float kpa);
float corrigir_pressao_por_temperatura(float pressao_kpa, float temp_c);
void criar_interface_modo_ambiente(void);
void criar_interface_completa(void);
void atualizar_display_ambiente(void);
void atualizar_display_completo(void);
void finalizar_medicao(void);
static esp_err_t root_get_handler(httpd_req_t *req);

/*
    VARIABLES
*/
cSMP3011    SMP3011;
CBMP280     BMP280;
SemaphoreHandle_t sensorMutex;

// Arrays para os pneus
static float pressao_pneu_abs_kpa[MAX_PNEUS] = {0};
static float temperatura_pneu_c[MAX_PNEUS] = {0};
static float pressao_pneu_manometrica_kpa[MAX_PNEUS] = {0};

// Ambiente
static float pressao_ambiente_kpa = 101.325f;
static float temperatura_ambiente_c = 25.0f;

// Estado da máquina
static estado_manometro_t estado_atual = ESTADO_AMBIENTE;
static int pneu_atual = 1;
static int total_pneus = 0;
static TickType_t tempo_sem_pressao = 0;
static TickType_t tempo_congelado_tela = 0;
static float ultima_pressao_absoluta = 0;

// Objetos globais do display (Corrigidos para evitar ponteiros nulos)
static lv_obj_t *lblStatus = NULL;
static lv_obj_t *lblAmbienteTemperatura = NULL;
static lv_obj_t *lblAmbientePressao = NULL;
static lv_obj_t *lblPneuInfo = NULL;
static lv_obj_t *lblPneuDados = NULL;

// Servidor web
static httpd_handle_t servidor_http = NULL;

// ========== FUNÇÕES DE UTILIDADE ==========

float kpa_to_psi(float kpa) {
    return kpa * 0.14503773773f;
}

float corrigir_pressao_por_temperatura(float pressao_kpa, float temp_c) {
    float temp_ref = 25.0f;
    float coef_temp = 0.002f;
    return pressao_kpa / (1.0f + coef_temp * (temp_c - temp_ref));
}

void finalizar_medicao(void) {
    if (estado_atual != ESTADO_AMBIENTE) {
        printf("\n=== MEDIÇÃO FINALIZADA CONTROLE DE CICLO ===\n");
        printf("Total de pneus medidos nesta sessao: %d\n", total_pneus);
        
        // Retorna o estado para ambiente
        estado_atual = ESTADO_AMBIENTE;
        
        // Força a recriação da interface estática de ambiente
        criar_interface_modo_ambiente();
        
        // Reseta variáveis de controle
        total_pneus = 0;
        pneu_atual = 1;
        tempo_sem_pressao = 0;
        tempo_congelado_tela = 0;
        ultima_pressao_absoluta = 0;
        
        // Limpa histórico de pneus
        for (int i = 0; i < MAX_PNEUS; i++) {
            pressao_pneu_manometrica_kpa[i] = 0;
            pressao_pneu_abs_kpa[i] = 0;
            temperatura_pneu_c[i] = 0;
        }
        
        // Feedback visual: Pisca LED de BOOT finalizado
        for (int i = 0; i < 5; i++) {
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        printf("✅ Sistema reiniciado com sucesso. Status: AGUARDANDO\n");
    }
}

// ========== DISPLAY (Otimizado e Protegido) ==========

void criar_interface_modo_ambiente(void) {
    lvgl_port_lock(portMAX_DELAY);
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    
    // Vincula diretamente às variáveis globais (Evita vazamento de memória e crash)
    lblStatus = lv_label_create(scr);
    lv_label_set_text(lblStatus, "AGUARDANDO ...");
    lv_obj_set_style_text_color(lblStatus, lv_color_white(), 0);
    lv_obj_set_pos(lblStatus, 0, 0);
    
    lblAmbienteTemperatura = lv_label_create(scr);
    lv_label_set_text_fmt(lblAmbienteTemperatura, "TAmb: %.1f C", temperatura_ambiente_c);
    lv_obj_set_style_text_color(lblAmbienteTemperatura, lv_color_white(), 0);
    lv_obj_set_pos(lblAmbienteTemperatura, 0, 24);
    
    lblAmbientePressao = lv_label_create(scr);
    lv_label_set_text_fmt(lblAmbientePressao, "PAmb: %.1f hPa", pressao_ambiente_kpa * 10.0f);
    lv_obj_set_style_text_color(lblAmbientePressao, lv_color_white(), 0);
    lv_obj_set_pos(lblAmbientePressao, 0, 44);
    
    // Reseta labels do pneu para evitar lixo de ponteiro
    lblPneuInfo = NULL;
    lblPneuDados = NULL;
    
    lvgl_port_unlock();
}

void criar_interface_completa(void) {
    lvgl_port_lock(portMAX_DELAY);
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    
    lblPneuInfo = lv_label_create(scr);
    lv_obj_set_style_text_color(lblPneuInfo, lv_color_white(), 0);
    lv_obj_set_pos(lblPneuInfo, 0, 0);
    
    lblPneuDados = lv_label_create(scr);
    lv_obj_set_style_text_color(lblPneuDados, lv_color_white(), 0);
    lv_obj_set_pos(lblPneuDados, 0, 32);
    
    // Reseta as de ambiente
    lblStatus = NULL;
    lblAmbienteTemperatura = NULL;
    lblAmbientePressao = NULL;
    
    lvgl_port_unlock();
    
    atualizar_display_completo();
}

void atualizar_display_ambiente(void) {
    lvgl_port_lock(portMAX_DELAY);
    // Só atualiza os textos se os ponteiros globais forem válidos
    if (lblAmbienteTemperatura && lblAmbientePressao) {
        lv_label_set_text_fmt(lblAmbienteTemperatura, "TAmb: %.1f C", temperatura_ambiente_c);
        lv_label_set_text_fmt(lblAmbientePressao, "PAmb: %.1f hPa", pressao_ambiente_kpa * 10.0f);
    }
    lvgl_port_unlock();
}

void atualizar_display_completo(void) {
    lvgl_port_lock(portMAX_DELAY);
    if (lblPneuInfo && lblPneuDados) {
        // Se voltamos para o modo estável sem retenção, pede para inserir o bico
        if (estado_atual == ESTADO_AGUARDANDO_PNEU && tempo_congelado_tela == 0) {
            lv_label_set_text_fmt(lblPneuInfo, "AGUARDANDO P%d", pneu_atual);
            lv_label_set_text(lblPneuDados, "Insira o bico...");
        } 
        else {
            // Define qual pneu mostrar: se estiver retendo a tela, mostra o anterior (pneu_atual - 2)
            int exibicao_idx = pneu_atual - 1;
            if (estado_atual == ESTADO_AGUARDANDO_PNEU && pneu_atual > 1) {
                exibicao_idx = pneu_atual - 2;
            }
            
            if (exibicao_idx < 0) exibicao_idx = 0;

            if (estado_atual == ESTADO_MEDINDO_PNEU) {
                lv_label_set_text_fmt(lblPneuInfo, "MEDINDO P%d...", exibicao_idx + 1);
            } else {
                lv_label_set_text_fmt(lblPneuInfo, "P%d CONCLUIDO", exibicao_idx + 1);
            }
            
            lv_label_set_text_fmt(lblPneuDados, "P%d: %.1f PSI  %.1fC", 
                                  exibicao_idx + 1, 
                                  kpa_to_psi(pressao_pneu_manometrica_kpa[exibicao_idx]), 
                                  temperatura_pneu_c[exibicao_idx]);
        }
    }
    lvgl_port_unlock();
}

// ========== SERVIDOR WEB (ESTÁVEL E DINÂMICO) ==========

static esp_err_t root_get_handler(httpd_req_t *req) {
    // Aumentado o buffer para 4096 bytes para suportar a tabela com múltiplos pneus
    char *response = (char *)malloc(4096);
    if (response == NULL) {
        printf("❌ Erro de alocação de memória para o HTML\n");
        return ESP_ERR_NO_MEM;
    }
    
    // Define a cor e o texto do badge de status atual
    const char* status_texto = "AGUARDANDO";
    const char* status_cor = "#28a745"; // Verde
    
    if (estado_atual == ESTADO_AGUARDANDO_PNEU) {
        status_texto = "AGUARDANDO PRÓXIMO PNEU";
        status_cor = "#ffc107"; // Amarelo
    } else if (estado_atual == ESTADO_MEDINDO_PNEU) {
        status_texto = "MEDINDO PNEU EM TEMPO REAL";
        status_cor = "#dc3545"; // Vermelho
    }

    // Parte 1 do HTML: Cabeçalho, Estilos e Bloco de Ambiente
    int offset = snprintf(response, 4096,
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<meta http-equiv='refresh' content='2'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<title>Manômetro Smart</title>"
        "<style>"
        "body { background:#121214; color:#e1e1e6; font-family:sans-serif; padding:15px; margin:0; text-align:center; }"
        ".container { max-width:480px; margin:0 auto; }"
        "h1 { color:#04d361; font-size:24px; margin-bottom:5px; }"
        ".status-badge { display:inline-block; padding:8px 15px; border-radius:20px; font-weight:bold; color:#000; font-size:14px; margin-bottom:15px; background:%s; }"
        ".card { background:#202024; padding:15px; border-radius:8px; margin-bottom:15px; border:1px solid #29292e; text-align:left; }"
        ".card h3 { margin:0 0 10px 0; color:#8d8d99; font-size:14px; text-transform:uppercase; letter-spacing:1px; }"
        ".card p { margin:5px 0; font-size:18px; font-weight:bold; }"
        "table { width:100%%; border-collapse:collapse; background:#202024; border-radius:8px; overflow:hidden; border:1px solid #29292e; }"
        "th { background:#29292e; color:#04d361; padding:12px; font-size:14px; text-align:left; }"
        "td { padding:12px; border-bottom:1px solid #29292e; font-size:16px; }"
        "tr:last-child td { border-bottom:none; }"
        ".no-data { text-align:center; color:#7c7c8a; padding:20px; font-style:italic; }"
        "</style>"
        "</head>"
        "<body>"
        "<div class='container'>"
        "<h1>🚗 Manômetro Inteligente</h1>"
        "<div class='status-badge'>%s</div>"
        "</div>"
        "<div class='card'>"
        "<h3>🌡️ Condições Ambientais (BMP280)</h3>"
        "<p>Temperatura: <span style='color:#ffb86c;'>%.1f °C</span></p>"
        "<p>Pressão Atmosférica: <span style='color:#8be9fd;'>%.1f hPa</span></p>"
        "</div>"
        "<div class='container' style='text-align:left; margin-bottom:10px;'>"
        "<h3 style='color:#8d8d99; font-size:14px; text-transform:uppercase; margin:0;'>📊 Histórico de Pneus Medidos</h3>"
        "</div>"
        "<table>"
        "<thead>"
        "<tr><th>Pneu</th><th>Pressão (PSI)</th><th>Temperatura</th></tr>"
        "</thead>"
        "<tbody>",
        status_cor, status_texto, temperatura_ambiente_c, (pressao_ambiente_kpa * 10.0f)
    );

    // Parte 2 do HTML: Popula a tabela com os pneus guardados na memória
    if (total_pneus == 0) {
        offset += snprintf(response + offset, 4096 - offset,
            "<tr><td colspan='3' class='no-data'>Nenhum pneu medido nesta sessão.</td></tr>"
        );
    } else {
        // Exibe todos os pneus coletados até o momento
        for (int i = 0; i < total_pneus; i++) {
            // Se for o pneu atual e estiver medindo ativamente, destaca a linha em amarelo
            const char* row_style = "";
            if ((i == pneu_atual - 1) && (estado_atual == ESTADO_MEDINDO_PNEU)) {
                row_style = " style='background:#29292e; font-weight:bold;'";
            }

            offset += snprintf(response + offset, 4096 - offset,
                "<tr%s>"
                "<td><b>P%d</b></td>"
                "<td style='color:#04d361;'>%.1f PSI</td>"
                "<td>%.1f °C</td>"
                "</tr>",
                row_style, (i + 1), kpa_to_psi(pressao_pneu_manometrica_kpa[i]), temperatura_pneu_c[i]
            );
        }
    }

    // Parte 3 do HTML: Fecha as tags finais da página
    snprintf(response + offset, 4096 - offset, "</tbody></table></div></body></html>");
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    
    // Libera a memória alocada do buffer dinâmico
    free(response);
    return ESP_OK;
}

void start_webserver(void) {
    // Inicializacao essencial do NVS para funcionamento da pilha Wi-Fi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, "PROJETO");
    wifi_config.ap.password[0] = '\0'; // Sem senha para conexao direta e rapida do celular
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN; 
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    printf("\n========================================\n");
    printf("🌐 Rede Wi-Fi Aberta Ativa: PROJETO\n");
    printf("📱 Conecte o celular e acesse: http://192.168.4.1\n");
    printf("========================================\n\n");
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 8192; // Aumentado para evitar estouro de pilha com HTML grande
    
    if (httpd_start(&servidor_http, &config) == ESP_OK) {
        httpd_uri_t uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(servidor_http, &uri);
        printf("✅ Servidor HTTP iniciado com sucesso!\n");
    } else {
        printf("❌ Falha crítica ao iniciar o Servidor HTTP\n");
    }
}

// ========== TAREFA DO BOTÃO DE BOOT (DEBOUNCE REFORÇADO) ==========

void botoesTask(void *pvParameters) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOTAO_FINALIZAR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    int contador_debounce = 0;
    
    while(1) {
        if (gpio_get_level(BOTAO_FINALIZAR_PIN) == 0) {
            contador_debounce++;
            if (contador_debounce >= 3) { // Exige 60ms de pressao constante para evitar ruidos
                printf("\n🔘 Botao BOOT detectado: Finalizando ciclo de medicao.\n");
                finalizar_medicao();
                contador_debounce = 0;
                vTaskDelay(pdMS_TO_TICKS(500)); // Evita multiplos disparos involuntarios
            }
        } else {
            contador_debounce = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ========== TAREFAS DOS SENSORES (MÁQUINA DE ESTADOS BLINDADA) ==========

// ========== TAREFAS DOS SENSORES (MÁQUINA DE ESTADOS CORRIGIDA) ==========

void sensorSMP3011Task(void *pvParameters) {
    float pressao_absoluta_leitura = 0;
    float temperatura_leitura = 0;
    int idx_pneu = 0;
    
    while(1) {
        SMP3011.poll();
        
        idx_pneu = pneu_atual - 1;
        if (idx_pneu >= 0 && idx_pneu < MAX_PNEUS) {
            pressao_absoluta_leitura = SMP3011.getPressure();
            temperatura_leitura = SMP3011.getTemperature();
            
            // CORREÇÃO: Como o sensor SMP3011 dá 0 em repouso e 8 no sopro,
            // a leitura corrigida por temperatura já é a própria pressão manométrica calculada!
            float pressao_manometrica_calculada = corrigir_pressao_por_temperatura(pressao_absoluta_leitura, temperatura_leitura);
            if (pressao_manometrica_calculada < 0) pressao_manometrica_calculada = 0;

            // CASO ESTEJA EM RETENÇÃO DE TELA (10 SEGUNDOS)
            if (estado_atual == ESTADO_AGUARDANDO_PNEU && tempo_congelado_tela > 0) {
                if ((xTaskGetTickCount() - tempo_congelado_tela) > pdMS_TO_TICKS(10000)) {
                    tempo_congelado_tela = 0;
                    atualizar_display_completo();
                    printf("⏳ Retencao encerrada. Tela pronta para o proximo pneu.\n");
                }
            }

            // DISPARO DE CONEXÃO (Ativo em AMBIENTE ou quando aguarda o próximo pneu)
            if (estado_atual == ESTADO_AMBIENTE || estado_atual == ESTADO_AGUARDANDO_PNEU) {
                if (pressao_manometrica_calculada > PRESSAO_LIMIAR_CONEXAO_KPA) {
                    estado_atual = ESTADO_MEDINDO_PNEU;
                    tempo_congelado_tela = 0; // Cancela retenção temporária se houver nova pressão imediata
                    total_pneus = pneu_atual;
                    
                    pressao_pneu_abs_kpa[idx_pneu] = pressao_absoluta_leitura;
                    temperatura_pneu_c[idx_pneu] = temperatura_leitura;
                    pressao_pneu_manometrica_kpa[idx_pneu] = pressao_manometrica_calculada;
                    
                    criar_interface_completa();
                    printf("\n✅ Pneu %d CONECTADO! Pressao Manometrica: %.2f kPa\n", pneu_atual, pressao_manometrica_calculada);
                }
            } 
            else if (estado_atual == ESTADO_MEDINDO_PNEU) {
                // Captura do valor de pico real durante o sopro/medição
                if (pressao_manometrica_calculada > pressao_pneu_manometrica_kpa[idx_pneu]) {
                    pressao_pneu_manometrica_kpa[idx_pneu] = pressao_manometrica_calculada;
                    pressao_pneu_abs_kpa[idx_pneu] = pressao_absoluta_leitura;
                    temperatura_pneu_c[idx_pneu] = temperatura_leitura;
                }
                
                atualizar_display_completo();
                
                // Filtro estável de Desconexão (Estouro de tempo sem pressão)
                if (pressao_manometrica_calculada < PRESSAO_LIMIAR_DESCONEXAO_KPA) {
                    if (tempo_sem_pressao == 0) {
                        tempo_sem_pressao = xTaskGetTickCount();
                    } else if ((xTaskGetTickCount() - tempo_sem_pressao) > pdMS_TO_TICKS(TEMPO_DESCONEXAO_MS)) {
                        printf("📊 Pneu %d Concluido: %.1f PSI\n", pneu_atual, kpa_to_psi(pressao_pneu_manometrica_kpa[idx_pneu]));
                        
                        tempo_sem_pressao = 0;
                        if (pneu_atual < MAX_PNEUS) {
                            pneu_atual++;
                            estado_atual = ESTADO_AGUARDANDO_PNEU;
                            tempo_congelado_tela = xTaskGetTickCount(); // Inicia cronômetro de 10s retendo os dados na tela
                            atualizar_display_completo();
                        } else {
                            finalizar_medicao();
                        }
                    }
                } else {
                    tempo_sem_pressao = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void sensorBMP280Task(void *pvParameters) {
    while(1) {
        BMP280.poll();
        pressao_ambiente_kpa = BMP280.getPressure();
        temperatura_ambiente_c = BMP280.getTemperature();
        
        if (estado_atual == ESTADO_AMBIENTE) {
            atualizar_display_ambiente();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void statusLedTask(void *pvParameters) {
    while(1) {
        switch (estado_atual) {
            case ESTADO_AMBIENTE:
                gpio_set_level(LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(1900));
                break;

            case ESTADO_AGUARDANDO_PNEU:
                gpio_set_level(LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(200));
                gpio_set_level(LED_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(800));
                break;

            case ESTADO_MEDINDO_PNEU:
                gpio_set_level(LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            default:
                gpio_set_level(LED_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(200));
                break;
        }
    }
}

// ========== MAIN ==========

extern "C" void app_main() {
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("I2C", ESP_LOG_ERROR);
    esp_log_level_set("i2c.master", ESP_LOG_ERROR);
    
    printf("\n\n=== MEDIDOR DE PRESSÃO DE PNEUS PORTÁTIL ===\n");
    printf("Operação: Conecte o bico ao pneu para registrar automaticamente.\n");
    printf("Botão BOOT (GPIO0): Finaliza o ciclo e limpa o painel.\n\n");
    
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);
    
    sensorMutex = xSemaphoreCreateMutex();
    
    I2C.init();
    SMP3011.init();
    BMP280.init();
    
    displayInit();
    vTaskDelay(pdMS_TO_TICKS(500)); 
    
    criar_interface_modo_ambiente();
    start_webserver();
    
    xTaskCreatePinnedToCore(statusLedTask, "statusLedTask", 2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(botoesTask, "botoesTask", 2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(sensorSMP3011Task, "sensorSMP3011Task", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(sensorBMP280Task, "sensorBMP280Task", 2048, NULL, 1, NULL, 1);
    
    while(1) {
        lvgl_port_lock(portMAX_DELAY);
        lv_timer_handler();
        lvgl_port_unlock();
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

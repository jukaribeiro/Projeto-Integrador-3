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

#define LED_PIN     gpio_num_t::GPIO_NUM_16  
#define BOTAO_FINALIZAR_PIN   gpio_num_t::GPIO_NUM_0

// Constantes para detecção de pneu
#define PRESSAO_LIMIAR_CONEXAO_KPA     2.0f    // Detecta subida > 2 kPa
#define PRESSAO_LIMIAR_DESCONEXAO_KPA  1.0f    // Detecta volta < 1 kPa
#define TEMPO_DESCONEXAO_MS            500     // Meio segundo
#define MAX_PNEUS 10

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
static int pneu_atual = 1;
static int total_pneus = 0;
static bool aguardando_conexao = true;
static bool medindo_pneu = false;
static bool medicao_finalizada = false;
static TickType_t tempo_sem_pressao = 0;
static float ultima_pressao_absoluta = 0;

// Objetos do display
static lv_obj_t *lblPneuPressao[MAX_PNEUS] = {NULL};
static lv_obj_t *lblPneuTemp[MAX_PNEUS] = {NULL};
static lv_obj_t *lblStatus = NULL;
static lv_obj_t *lblAmbiente = NULL;
static lv_obj_t *lblAmbienteTemperatura = NULL;
static lv_obj_t *lblAmbientePressao = NULL;
static lv_obj_t *lblContador = NULL;

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
    if (!medicao_finalizada && total_pneus > 0) {
        medicao_finalizada = true;
        aguardando_conexao = false;
        medindo_pneu = false;
        
        printf("\n=== MEDIÇÃO FINALIZADA ===\n");
        printf("Total de pneus: %d\n", total_pneus);
        for (int i = 0; i < total_pneus; i++) {
            printf("Pneu %d: %.1f PSI | %.1f C\n", 
                   i+1, kpa_to_psi(pressao_pneu_manometrica_kpa[i]), temperatura_pneu_c[i]);
        }
        
        // Pisca LED 5 vezes
        for (int i = 0; i < 5; i++) {
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ========== DISPLAY ==========

void criar_interface_modo_ambiente(void) {
    lvgl_port_lock(portMAX_DELAY);
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    
    if (lblAmbienteTemperatura) {
        lv_obj_del(lblAmbienteTemperatura);
        lv_obj_del(lblAmbientePressao);
        lv_obj_del(lblStatus);
        lv_obj_del(lblContador);
    }
    
    // Fundo preto
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    
    // Título
    lv_obj_t *lblTitulo = lv_label_create(scr);
    lv_label_set_text(lblTitulo, "MEDIDOR DE PNEUS");
    lv_obj_set_style_text_color(lblTitulo, lv_color_white(), 0);
    lv_obj_set_pos(lblTitulo, 0, 0);
    
    // Contador
    lblContador = lv_label_create(scr);
    if (total_pneus > 0) {
        lv_label_set_text_fmt(lblContador, "Pneus medidos: %d", total_pneus);
    } else {
        lv_label_set_text(lblContador, "Aguardando...");
    }
    lv_obj_set_style_text_color(lblContador, lv_color_white(), 0);
    lv_obj_set_pos(lblContador, 0, 16);
    
    // Temperatura Ambiente
    lblAmbienteTemperatura = lv_label_create(scr);
    lv_label_set_text_fmt(lblAmbienteTemperatura, "TEMP: %.1f C", temperatura_ambiente_c);
    lv_obj_set_style_text_color(lblAmbienteTemperatura, lv_color_white(), 0);
    lv_obj_set_pos(lblAmbienteTemperatura, 0, 40);
    
    // Pressão Ambiente
    lblAmbientePressao = lv_label_create(scr);
    lv_label_set_text_fmt(lblAmbientePressao, "PRESS: %.1f kPa", pressao_ambiente_kpa);
    lv_obj_set_style_text_color(lblAmbientePressao, lv_color_white(), 0);
    lv_obj_set_pos(lblAmbientePressao, 0, 56);
    
    lvgl_port_unlock();
}

void criar_interface_completa(void) {
    lvgl_port_lock(portMAX_DELAY);
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    
    for (int i = 0; i < MAX_PNEUS; i++) {
        if (lblPneuPressao[i]) {
            lv_obj_del(lblPneuPressao[i]);
            lv_obj_del(lblPneuTemp[i]);
        }
    }
    if (lblAmbiente) lv_obj_del(lblAmbiente);
    if (lblStatus) lv_obj_del(lblStatus);
    if (lblContador) lv_obj_del(lblContador);
    
    // Fundo preto
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    
    // Contador no topo
    lblContador = lv_label_create(scr);
    lv_label_set_text_fmt(lblContador, "Pneus medidos: %d", total_pneus);
    lv_obj_set_style_text_color(lblContador, lv_color_white(), 0);
    lv_obj_set_pos(lblContador, 0, 0);
    
    // Lista de pneus
    int y_pos = 16;
    for (int i = 0; i < total_pneus && i < 4; i++) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "P%d: %.1f PSI  %.0fC", 
                 i+1, kpa_to_psi(pressao_pneu_manometrica_kpa[i]), temperatura_pneu_c[i]);
        
        lblPneuPressao[i] = lv_label_create(scr);
        lv_label_set_text(lblPneuPressao[i], buffer);
        lv_obj_set_style_text_color(lblPneuPressao[i], lv_color_white(), 0);
        lv_obj_set_pos(lblPneuPressao[i], 0, y_pos);
        y_pos += 16;
    }
    
    // Ambiente
    lblAmbiente = lv_label_create(scr);
    lv_label_set_text_fmt(lblAmbiente, "AMB: %.0fC  %.0fhPa", 
                          temperatura_ambiente_c, pressao_ambiente_kpa);
    lv_obj_set_style_text_color(lblAmbiente, lv_color_white(), 0);
    lv_obj_set_pos(lblAmbiente, 0, 54);
    
    lvgl_port_unlock();
}

void atualizar_display_ambiente(void) {
    lvgl_port_lock(portMAX_DELAY);
    if (lblAmbienteTemperatura) {
        lv_label_set_text_fmt(lblAmbienteTemperatura, "TEMP: %.1f C", temperatura_ambiente_c);
        lv_label_set_text_fmt(lblAmbientePressao, "PRESS: %.1f kPa", pressao_ambiente_kpa);
    }
    if (lblContador && total_pneus > 0) {
        lv_label_set_text_fmt(lblContador, "Pneus medidos: %d", total_pneus);
    }
    lvgl_port_unlock();
}

void atualizar_display_completo(void) {
    lvgl_port_lock(portMAX_DELAY);
    for (int i = 0; i < total_pneus && i < 4; i++) {
        if (lblPneuPressao[i]) {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "P%d: %.1f PSI  %.0fC", 
                     i+1, kpa_to_psi(pressao_pneu_manometrica_kpa[i]), temperatura_pneu_c[i]);
            lv_label_set_text(lblPneuPressao[i], buffer);
        }
    }
    if (lblAmbiente) {
        lv_label_set_text_fmt(lblAmbiente, "AMB: %.0fC  %.0fhPa", 
                              temperatura_ambiente_c, pressao_ambiente_kpa);
    }
    if (lblContador) {
        lv_label_set_text_fmt(lblContador, "Pneus medidos: %d", total_pneus);
    }
    lvgl_port_unlock();
}

// ========== SERVIDOR WEB (VERSÃO ESTÁVEL) ==========

static esp_err_t root_get_handler(httpd_req_t *req) {
    // Aumenta o buffer para 2048 bytes
    char response[2048];
    
    snprintf(response, sizeof(response),
        "<!DOCTYPE html>"
        "<html>"
        "<head><meta charset='UTF-8'><meta http-equiv='refresh' content='3'></head>"
        "<body style='background:#1a1a2e;color:white;font-family:Arial;padding:20px;'>"
        "<h1>🚗 Monitor de Pneus</h1>"
        "<div style='background:#16213e;padding:15px;border-radius:10px;margin:10px 0;'>"
        "🌡️ Ambiente: %.1f C | %.1f PSI</div>"
        "<div style='background:#0f3460;padding:15px;border-radius:10px;margin:10px 0;'>"
        "Pneus medidos: %d</div>"
        "<div style='background:#0f3460;padding:15px;border-radius:10px;'>"
        "Status: %s</div>"
        "</body></html>",
        temperatura_ambiente_c, kpa_to_psi(pressao_ambiente_kpa),
        total_pneus,
        medicao_finalizada ? "Finalizado" : "Medindo"
    );
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

void start_webserver(void) {
    // Inicializa rede
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    
    // Configura WiFi no modo Access Point
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Configuração do Access Point
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, "PROJETO");
    wifi_config.ap.ssid_len = 0;
    strcpy((char*)wifi_config.ap.password, "12345678");
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    printf("\n========================================\n");
    printf("✅ Rede WiFi: PROJETO\n");
    printf("🔑 Senha: 12345678\n");
    printf("🌐 Acesse: http://192.168.4.1\n");
    printf("========================================\n\n");
    
    // Inicia servidor HTTP com configuração estável
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 6144;  // Stack size adequado
    
    if (httpd_start(&servidor_http, &config) == ESP_OK) {
        httpd_uri_t uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(servidor_http, &uri);
        printf("✅ Servidor HTTP iniciado!\n");
    } else {
        printf("❌ Erro ao iniciar servidor HTTP\n");
    }
}

// ========== TAREFA DO BOTÃO ==========

void botoesTask(void *pvParameters) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOTAO_FINALIZAR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    bool ultimo_estado = true;
    
    while(1) {
        bool estado_atual = gpio_get_level(BOTAO_FINALIZAR_PIN);
        
        if (estado_atual == 0 && ultimo_estado == 1) {
            printf("\n🔘 Botao pressionado!\n");
            finalizar_medicao();
        }
        
        ultimo_estado = estado_atual;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ========== TAREFAS DOS SENSORES ==========

void sensorSMP3011Task(void *pvParameters) {
    float pressao_atual_manometrica = 0;
    int idx_pneu = 0;
    
    while(1) {
        SMP3011.poll();
        
        if (!medicao_finalizada && pneu_atual <= MAX_PNEUS) {
            idx_pneu = pneu_atual - 1;
            pressao_pneu_abs_kpa[idx_pneu] = SMP3011.getPressure();
            temperatura_pneu_c[idx_pneu] = SMP3011.getTemperature();
            
            float pressao_corrigida = corrigir_pressao_por_temperatura(
                pressao_pneu_abs_kpa[idx_pneu], temperatura_pneu_c[idx_pneu]);
            
            pressao_atual_manometrica = pressao_corrigida - pressao_ambiente_kpa;
            
            // Print simplificado a cada 10 leituras para não poluir
            static int contador_print = 0;
            if (++contador_print >= 10) {
                printf("P%d: Abs=%.2f kPa | Amb=%.2f | Man=%.2f | Temp=%.1fC\n", 
                       pneu_atual,
                       pressao_pneu_abs_kpa[idx_pneu],
                       pressao_ambiente_kpa,
                       pressao_atual_manometrica,
                       temperatura_pneu_c[idx_pneu]);
                contador_print = 0;
            }
        }
        
        // MÁQUINA DE ESTADOS - USANDO PRESSÃO ABSOLUTA
        if (!medicao_finalizada && idx_pneu >= 0) {
            float pressao_abs_atual = pressao_pneu_abs_kpa[idx_pneu];
            
            if (aguardando_conexao) {
                // Detecta conexão quando pressão absoluta > 2 kPa
                if (pressao_abs_atual > PRESSAO_LIMIAR_CONEXAO_KPA) {
                    aguardando_conexao = false;
                    medindo_pneu = true;
                    tempo_sem_pressao = 0;
                    
                    if (pneu_atual > total_pneus) {
                        total_pneus = pneu_atual;
                    }
                    
                    printf("\n✅ Pneu %d CONECTADO! Pressão Abs = %.2f kPa\n", pneu_atual, pressao_abs_atual);
                    
                    if (pneu_atual == 1 && total_pneus == 1) {
                        criar_interface_completa();
                    }
                }
            } 
            else if (medindo_pneu) {
                // Usa pressão absoluta para a leitura do pneu
                pressao_pneu_manometrica_kpa[idx_pneu] = pressao_abs_atual;
                
                // Detecta desconexão quando pressão volta perto de 0
                if (pressao_abs_atual < PRESSAO_LIMIAR_DESCONEXAO_KPA) {
                    if (tempo_sem_pressao == 0) {
                        tempo_sem_pressao = xTaskGetTickCount();
                    } else if ((xTaskGetTickCount() - tempo_sem_pressao) > pdMS_TO_TICKS(TEMPO_DESCONEXAO_MS)) {
                        medindo_pneu = false;
                        aguardando_conexao = true;
                        
                        printf("📊 Pneu %d FINALIZADO: %.1f PSI | %.1f C\n", 
                               pneu_atual, kpa_to_psi(pressao_pneu_manometrica_kpa[idx_pneu]), 
                               temperatura_pneu_c[idx_pneu]);
                        
                        pneu_atual++;
                        tempo_sem_pressao = 0;
                        
                        if (pneu_atual <= MAX_PNEUS) {
                            printf("⏳ Aguardando pneu %d...\n", pneu_atual);
                        }
                    }
                } else {
                    tempo_sem_pressao = 0;
                }
            }
        }
        
        ultima_pressao_absoluta = pressao_pneu_abs_kpa[idx_pneu];
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void sensorBMP280Task(void *pvParameters) {
    while(1) {
        BMP280.poll();
        pressao_ambiente_kpa = BMP280.getPressure();
        temperatura_ambiente_c = BMP280.getTemperature();
        vTaskDelay(pdMS_TO_TICKS(2000));  // Lê a cada 2 segundos
    }
}

void statusLedTask(void *pvParameters) {
    while(1) {
        if (medicao_finalizada) {
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(1900));
        } 
        else if (aguardando_conexao) {
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(800));
        } 
        else if (medindo_pneu) {
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else {
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ========== MAIN ==========

extern "C" void app_main() {
    // Configura níveis de log
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("I2C", ESP_LOG_ERROR);
    esp_log_level_set("i2c.master", ESP_LOG_ERROR);
    
    printf("\n\n=== MEDIDOR DE PRESSAO DE PNEUS ===\n");
    printf("Modo: Conecte e desconecte os pneus\n");
    printf("Botao (GPIO0): finaliza a medicao\n\n");
    
    // Inicializa NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // LED
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    xTaskCreate(statusLedTask, "statusLedTask", 2048, NULL, 1, NULL);
    
    // Botão
    xTaskCreate(botoesTask, "botoesTask", 2048, NULL, 1, NULL);
    
    // I2C e Sensores
    I2C.init();
    SMP3011.init();
    BMP280.init();
    sensorMutex = xSemaphoreCreateMutex();
    xTaskCreate(sensorSMP3011Task, "sensorSMP3011Task", 4096, NULL, 1, NULL);
    xTaskCreate(sensorBMP280Task, "sensorBMP280Task", 2048, NULL, 1, NULL);
    
    // Display
    displayInit();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Servidor Web
    start_webserver();
    
    // Interface inicial
    criar_interface_modo_ambiente();
    
    // Loop principal
    while(1) {
        if (medicao_finalizada && total_pneus > 0) {
            if (lblPneuPressao[0] != NULL) {
                atualizar_display_completo();
            }
        } 
        else if (total_pneus == 0 || aguardando_conexao) {
            atualizar_display_ambiente();
        } 
        else if (medindo_pneu && total_pneus > 0) {
            if (lblPneuPressao[0] == NULL) {
                criar_interface_completa();
            }
            atualizar_display_completo();
        }
        
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
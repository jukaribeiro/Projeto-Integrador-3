# PROJETO INTEGRADOR III
## Manômetro para Pneus

Autor: Julio C L Ribeiro  

### 🔑 Características principais  
* **Leituras de Pressão individualizadas:** Leitura da pressão de mais de um pneu, com dados individualizados na tela, levando em consideração as devidas compensações de temperatura e pressão ambiente.
* **Conectividade Sem Fio:** Envio dos dados coletados para dispositivos móveis conectados via Wi-Fi própria dispensando internet.
* **Respeito a biblioteca fornecida:** Modificamos apenas os arquivos main.cpp e o CMakeList.txt para a compilação. Obs.: Uma única modificação em displaySSD1306.c foi para inverter o preto/branco.

### 📱 Telas mostradas no dispositivo móvel

Exemplo de telas exibidas em um celular celular conectado, durante os principais estados:

| Aguardando Leituras | Leituras Concluídas |
| :---: | :---: |
| ![Tela aguardando leituras](figura1.png) <br> *Aguardando conexão com os sensores* | ![Tela com leituras feitas](figura2.png) <br> *Dados de pressão atualizados* |

---

[Clique aqui para ver o código fonte deste projeto](https://github.com/jukaribeiro/Projeto-Integrador-3)

[Clique aqui para ver o Vídeo deste projeto](https://youtu.be/7gpSlrYxqJQ)

## 📚 Funções utilizadas

### 🔧 Fuções de corverção e correção

| Função | Finalidade |
|--------|------------|
| `kpa_to_psi()` | Converte pressão de kPa para PSI (fator 0.14503773773) |
| `corrigir_pressao_por_temperatura()` | Aplica correção térmica com coeficiente de 0,2%/°C (referência 25°C) |
| `finalizar_medicao()` | Reseta o ciclo completo: volta ao estado `AMBIENTE`, limpa memória dos pneus, recria interface e pisca o LED 5x como feedback visual |

---

### 🖥️ Display (LVGL)

| Função | Finalidade |
|--------|------------|
| `criar_interface_modo_ambiente()` | Monta tela inicial com temperatura e pressão ambiente, label de status "AGUARDANDO ..." |
| `criar_interface_completa()` | Monta interface para mostrar medição do pneu atual (número, PSI, temperatura) |
| `atualizar_display_ambiente()` | Atualiza em tempo real os valores de T e P ambiente na tela |
| `atualizar_display_completo()` | Mostra pneu em medição ou último concluído, com retenção de tela de 10s após desconexão |

---

### 🌐 Servidor Web

| Função | Finalidade |
|--------|------------|
| `start_webserver()` | Inicializa NVS, Wi-Fi em modo AP aberto (SSID: `PROJETO`), e sobe servidor HTTP na porta 80 |
| `root_get_handler()` | Gera página HTML dinâmica com badge de status colorido, dados ambientais e tabela com todos os pneus medidos na sessão |

---

### 🎮 Entrada/Saída

| Função | Finalidade |
|--------|------------|
| `botoesTask()` | Monitora botão BOOT (GPIO0) com debounce de ~60ms; ao pressionar, chama `finalizar_medicao()` |
| `statusLedTask()` | Controla LED indicador conforme estado: pisca lento (ambiente), médio (aguardando) ou acesso fixo (medindo) |

---

### 📡 Sensores

| Função | Finalidade |
|--------|------------|
| `sensorSMP3011Task()` | **Núcleo do projeto.** Lê sensor de pressão dos pneus a cada 100ms e implementa máquina de estados com detecção automática de conexão/desconexão, captura de pico e avanço entre pneus |
| `sensorBMP280Task()` | Lê sensor ambiental (temperatura e pressão atmosférica) a cada 2s e atualiza display no estado `AMBIENTE` |

---



### 📊 Tabela de Transições

| De | Para | Condição | Ação |
|----|------|----------|------|
| `AMBIENTE` | `MEDINDO_PNEU` | Pressão manométrica > 3 kPa | Cria interface completa, inicia captura de pico |
| `MEDINDO_PNEU` | `AGUARDANDO_PNEU` | Pressão < 1 kPa por 500ms contínuos | Congela tela por 10s, incrementa contador de pneus |
| `AGUARDANDO_PNEU` | `MEDINDO_PNEU` | Nova pressão > 3 kPa **E** pneu_atual < MAX_PNEUS | Avança para próximo pneu |
| `AGUARDANDO_PNEU` | `AMBIENTE` | Pneu_atual >= MAX_PNEUS **OU** botão BOOT pressionado | Chama `finalizar_medicao()` |
| `AMBIENTE` | `AMBIENTE` | Botão BOOT pressionado | Também chama `finalizar_medicao()` (reset completo) |

### ⏱️ Temporizações Importantes

| Evento | Tempo | Descrição |
|--------|-------|-----------|
| Retenção de tela | 10 segundos | Após concluir um pneu, mantém dados visíveis antes de mostrar "Insira o bico..." |
| Filtro de desconexão | 500 ms | Evita que oscilações rápidas de pressão disparem falsa desconexão |
| Timeout da retenção | 10 segundos | Se nenhum novo pneu for conectado, a tela volta a mostrar "AGUARDANDO Pn" |


| Estado | Comportamento do LED |
|--------|----------------------|
| `AMBIENTE` | Pisca lento (100ms ligado / 1900ms desligado) |
| `AGUARDANDO_PNEU` | Pisca médio (200ms ligado / 800ms desligado) |
| `MEDINDO_PNEU` | Acesso fixo enquanto mede |

---

### 🌐 Acesso ao Servidor Web

1. Conecte o celular à rede Wi-Fi **`PROJETO`** 
2. Acesse o endereço: **`http://192.168.4.1`**
3. A página atualiza automaticamente a cada 2 segundos

**Informações exibidas na página web:**
- Badge de status colorido (verde/amarelo/vermelho)
- Temperatura e pressão ambiente (BMP280)
- Tabela com histórico de todos os pneus medidos (PSI e °C)
- Destaque amarelo no pneu em medição ativa

---

### 📌 Constantes de Configuração

| Constante | Valor | Descrição |
|-----------|-------|-----------|
| `PRESSAO_LIMIAR_CONEXAO_KPA` | 3.0 | Pressão manométrica mínima para detectar conexão do bico |
| `PRESSAO_LIMIAR_DESCONEXAO_KPA` | 1.0 | Pressão abaixo da qual se considera desconectado |
| `TEMPO_DESCONEXAO_MS` | 500 | Tempo de filtro para confirmar desconexão (evita falsos disparos) |
| `MAX_PNEUS` | 10 | Número máximo de pneus por ciclo de medição |

## 📁 Estrutura de Arquivos do Projeto

### ✅ Arquivos Utilizados

| Arquivo | Tipo | Como foi utilizado |
|---------|------|-------------------|
| `./main/main.cpp` | Código Fonte | **Arquivo principal do firmware** com todas as tasks, máquina de estados e servidor web |
| `./main/cSMP3011.h` | Header | Interface da classe do sensor de pressão dos pneus (SMP3011) |
| `./main/cSMP3011.cpp` | Código Fonte | Implementação da leitura do sensor SMP3011 via I2C |
| `./main/CBMP280.h` | Header | Interface da classe do sensor de temperatura/pressão ambiente (BMP280) |
| `./main/CBMP280.cpp` | Código Fonte | Implementação da leitura do sensor BMP280 via I2C |
| `./main/CGlobalResources.h` | Header | Definições globais compartilhadas (objeto I2C, semáforos, etc.) |
| `./main/CGlobalResources.cpp` | Código Fonte | Implementação dos recursos globais do sistema |
| `./main/CI2C.h` | Header | Interface da classe de comunicação I2C |
| `./main/CI2C.cpp` | Código Fonte | Implementação do barramento I2C (inicialização, leitura/escrita) |
| `./main/displaySSD1306.h` | Header | Interface das funções de controle do display OLED SSD1306 |
| `./main/displaySSD1306.c` | Código Fonte | Implementação do display (inicialização, integração com LVGL) |
| `./main/idf_component.yml` | Configuração | Gerenciador de dependências do ESP-IDF (LVGL, drivers, etc.) |


### 🔗 Relações de Inclusão e Compilação

| Arquivo Principal | Inclui | Depende da Implementação |
|-------------------|--------|---------------------------|
| `main.cpp` | `cSMP3011.h` | `cSMP3011.cpp` |
| `main.cpp` | `CBMP280.h` | `CBMP280.cpp` |
| `main.cpp` | `CGlobalResources.h` | `CGlobalResources.cpp` |
| `CGlobalResources.h` | `CI2C.h` | `CI2C.cpp` |
| `main.cpp` | `displaySSD1306.h` | `displaySSD1306.c` |


### 📝 Descrição dos Arquivos Principais

#### 🔧 `main.cpp` (Arquivo Principal)
- Implementa a máquina de estados completa
- Cria e gerencia todas as tasks FreeRTOS:
  - `sensorSMP3011Task`
  - `sensorBMP280Task`
  - `statusLedTask`
  - `botoesTask` 
- Inicializa servidor web Wi-Fi
- Contém toda a lógica de interface LVGL

#### 📡 Sensores

| Classe | Sensor | Protocolo | Função |
|--------|--------|-----------|--------|
| `cSMP3011` | SMP3011 | I2C | Medição de pressão manométrica e temperatura dos pneus |
| `CBMP280` | BMP280 | I2C | Medição de pressão atmosférica e temperatura ambiente |

#### 🖥️ Display
- **Driver**: SSD1306 (OLED 128x64)
- **Biblioteca gráfica**: LVGL 
- **Comunicação**: I2C 

#### 🌐 Comunicação
- **Wi-Fi**: Modo Access Point (AP)
- **SSID**: `PROJETO` (rede provisoriamente aberta para facilitar a apresentação)
- **Servidor Web**: HTTP na porta 80
- **Endpoint**: `http://192.168.4.1/`


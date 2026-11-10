/*====================================================
Bibliotecas a instalar:
- Adafruit GFX Library;
- Adafruit ST7735 and ST7789 Library;
- WiFiManager (by tzapu).
====================================================*/

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "pagina.h"

/*====================================================
                  1. CONF DOS ALARMES
====================================================*/

#define MAX_ALARMES 10 // Quantos alarmes podem ser criados.
#define PADRAO       0 // Recorrente por intervalo de horas ou mesma hora todo dia
#define DIAS         1 // Mesma hora em determinados dias da semana

/*====================================================
                      2. PINAGEM
====================================================*/

//Display
#define TFT_SCK 12
#define TFT_SDA 11
#define TFT_CS  10
#define TFT_RS   9
#define TFT_RST  8
#define TFT_LED  7

//Buzzers e fim de curso
#define BUZZER1 38
#define BUZZER2 39
#define BUZZER3 40

//Botões e sensor de fim de curso
#define BOTAO  0 // Interligado ao BOOT, pressionar reinicia a configuração de Wi-Fi.
#define TAMPA 21 // Sensor para detectar estado da tampa.

/*====================================================
                      3. VARIÁVEIS
====================================================*/

Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_RS, TFT_RST);
WebServer server(80);
Preferences memoria;

//Parâmetros dos alarmes
struct Alarme {
  uint8_t usado;     // 1 = Posição ocupada
  uint8_t tipo;      // PADRAO ou DIAS
  uint8_t hora;      // 0 a 23
  uint8_t minuto;    // 0 a 59
  uint8_t intervalo; // Tipo padrão: intervalo de horas
  char dias[8];      // Tipo dias: 7 letras '0' ou '1' (Seg Ter Qua Qui Sex Sab Dom)
  char nome[33];     // Nome do remédio
};

Alarme alarmes[MAX_ALARMES]; // Lista dos alarmes

bool tocando = false;        // Verdadeiro enquanto o alarme está tocando.
bool tampaJaFechou = false;  // Verdadeiro quando a tampa é fechada durante o alarme.
int ultimoMinuto = -1;       // Último minuto em que conferiu os alarmes.
int ultimoSegundo = -1;      // Para só redesenhar a hora quando o segundo muda.

// ================== 4. FUNCOES: ALARMES NA MEMORIA ==================

void carregarAlarmes() {
  memset(alarmes, 0, sizeof(alarmes));
  if (memoria.getBytesLength("lista") == sizeof(alarmes)) {
    memoria.getBytes("lista", alarmes, sizeof(alarmes));
  }
}

void salvarAlarmes() {
  memoria.putBytes("lista", alarmes, sizeof(alarmes));
}

// Copia o nome deixando so caracteres simples (sem aspas, barras ou acentos)
void copiarNome(char* destino, String origem) {
  origem.trim();
  int n = 0;
  for (unsigned int i = 0; i < origem.length() && n < 32; i++) {
    char c = origem[i];
    if (c >= 32 && c <= 126 && c != '"' && c != '\\') {
      destino[n++] = c;
    }
  }
  destino[n] = '\0';
}

// ================== 5. FUNCOES: HORA E ALARMES ==================

// So vale depois que a internet acertou o relogio
bool horaValida() {
  return time(nullptr) > 1700000000UL;
}

// Esta na hora deste alarme tocar?
//   minutoDoDia = minutos desde a meia-noite (ex.: 08:30 = 510)
//   diaSemana   = 0 (segunda) ate 6 (domingo)
bool alarmeEstaNaHora(const Alarme& a, int minutoDoDia, int diaSemana) {
  int minutoDoAlarme = a.hora * 60 + a.minuto;

  // Tipo dias da semana: toca no horario do alarme, so nos dias marcados
  if (a.tipo == DIAS) {
    return a.dias[diaSemana] == '1' && minutoDoDia == minutoDoAlarme;
  }

  // Tipo padrao: toca no horario do alarme e depois a cada 'intervalo' horas.
  // (com intervalo 24, toca so uma vez por dia)
  if (a.intervalo == 0) return false;
  int passou = (minutoDoDia - minutoDoAlarme + 1440) % 1440;   // minutos desde o horario do alarme
  return passou % (a.intervalo * 60) == 0;
}

// ================== 6. FUNCOES: DISPLAY ==================

// Tela normal: fundo preto, endereco do site embaixo e letra grande para a hora
void telaRelogio() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.setCursor(5, 118);
  tft.print("Site: ");
  tft.print(WiFi.localIP());
  tft.setTextSize(3);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  ultimoSegundo = -1;
}

// Tela do alarme: fundo vermelho com o nome do remedio
void telaAlarme(const char* nome) {
  tft.fillScreen(ST77XX_RED);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE, ST77XX_RED);
  tft.setCursor(30, 10);
  tft.print("TOME O");
  tft.setCursor(30, 30);
  tft.print("REMEDIO!");

  tft.setTextColor(ST77XX_YELLOW, ST77XX_RED);
  tft.setTextSize(strlen(nome) * 12 <= 150 ? 2 : 1);   // nome comprido usa letra menor
  tft.setCursor(5, 75);
  tft.print(nome);
}

// Dica no pe da tela do alarme: aparece se a tampa ja estava aberta quando o alarme tocou
void dicaTampa(bool mostrar) {
  tft.fillRect(0, 105, tft.width(), 23, ST77XX_RED);   // apaga a area da dica
  if (mostrar) {
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE, ST77XX_RED);
    tft.setCursor(5, 112);
    tft.print("Feche e abra a tampa");
  }
}

// Instrucoes mostradas quando o ESP32 abre o Wi-Fi de configuracao
void telaConfigurarWifi(WiFiManager* wm) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(5, 5);
  tft.print("CONFIGURAR WI-FI");
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(5, 25);
  tft.print("1) Em outro aparelho,");
  tft.setCursor(5, 36);
  tft.print("acesse o wifi:");
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(5, 50);
  tft.print("ClockMedio");
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(5, 80);
  tft.print("2) Acesse no navegador:");
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(5, 95);
  tft.print("conectar.com");
}

// Escreve a hora no display, no formato HH:MM:SS (so quando o segundo muda)
void mostrarHora(struct tm& t) {
  if (t.tm_sec == ultimoSegundo) return;
  ultimoSegundo = t.tm_sec;

  char texto[9];
  sprintf(texto, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  tft.setCursor(8, 50);
  tft.print(texto);
}

// ================== 7. FUNCOES: BUZZER, TAMPA E BOTAO ==================

// Liga os 3 buzzers fazendo um som por 'tempo' milissegundos.
// O pino liga e desliga bem rapido (cerca de 2700 vezes por segundo) para gerar o som.
void bipe(int tempo) {
  unsigned long fim = millis() + tempo;
  while (millis() < fim) {
    digitalWrite(BUZZER1, HIGH);
    digitalWrite(BUZZER2, HIGH);
    digitalWrite(BUZZER3, HIGH);
    delayMicroseconds(185);
    digitalWrite(BUZZER1, LOW);
    digitalWrite(BUZZER2, LOW);
    digitalWrite(BUZZER3, LOW);
    delayMicroseconds(185);
  }
}

// Tampa aberta = circuito aberto = pino le HIGH
bool tampaAberta() {
  return digitalRead(TAMPA) == HIGH;
}

// Segurar o botao BOOT por 3 segundos apaga o Wi-Fi salvo e reinicia
void verificarBotaoWifi() {
  if (digitalRead(BOTAO) != LOW) return;

  unsigned long inicio = millis();
  while (digitalRead(BOTAO) == LOW) {
    if (millis() - inicio > 3000) {
      tft.fillScreen(ST77XX_BLACK);
      tft.setTextSize(1);
      tft.setTextColor(ST77XX_YELLOW);
      tft.setCursor(5, 50);
      tft.print("Resetando Wi-Fi...");

      WiFiManager wm;
      wm.resetSettings();
      delay(1000);
      ESP.restart();
    }
    delay(20);
  }
}

// ================== 8. FUNCOES: SITE ==================

// Entrega a pagina ao celular (o HTML esta no arquivo pagina.h)
void siteInicio() {
  server.send(200, "text/html; charset=utf-8", PAGINA);
}

// Entrega a lista de alarmes (a pagina usa isto para desenhar os cartoes)
void siteListar() {
  String resposta = "[";
  bool primeiro = true;
  for (int i = 0; i < MAX_ALARMES; i++) {
    if (!alarmes[i].usado) continue;
    if (!primeiro) resposta += ",";
    primeiro = false;
    resposta += "{\"id\":" + String(i) +
                ",\"tipo\":" + String(alarmes[i].tipo) +
                ",\"hora\":" + String(alarmes[i].hora) +
                ",\"minuto\":" + String(alarmes[i].minuto) +
                ",\"intervalo\":" + String(alarmes[i].intervalo) +
                ",\"dias\":\"" + String(alarmes[i].dias) + "\"" +
                ",\"nome\":\"" + String(alarmes[i].nome) + "\"}";
  }
  resposta += "]";
  server.send(200, "application/json", resposta);
}

// Cria um alarme novo (id = -1) ou edita um existente
void siteSalvar() {
  if (!server.hasArg("hora") || !server.hasArg("minuto") || !server.hasArg("tipo")) {
    server.send(400, "text/plain", "Dados incompletos.");
    return;
  }
  int id = server.hasArg("id") ? server.arg("id").toInt() : -1;
  int hora = server.arg("hora").toInt();
  int minuto = server.arg("minuto").toInt();
  int tipo = server.arg("tipo").toInt();
  int intervalo = server.hasArg("intervalo") ? server.arg("intervalo").toInt() : 24;
  String dias = server.arg("dias");
  String nome = server.arg("nome");

  // Confere se os valores fazem sentido
  if (hora < 0 || hora > 23 || minuto < 0 || minuto > 59) {
    server.send(400, "text/plain", "Horário inválido.");
    return;
  }
  if (tipo == PADRAO) {
    // O intervalo precisa dividir 24 horas certinho: 1, 2, 3, 4, 6, 8, 12 ou 24
    if (intervalo < 1 || intervalo > 24 || 24 % intervalo != 0) {
      server.send(400, "text/plain", "Intervalo inválido.");
      return;
    }
  } else if (tipo == DIAS) {
    if (dias.length() != 7 || dias.indexOf('1') < 0) {
      server.send(400, "text/plain", "Escolha pelo menos um dia da semana.");
      return;
    }
    intervalo = 24;
  } else {
    server.send(400, "text/plain", "Tipo inválido.");
    return;
  }

  // Alarme novo: procura uma posicao livre na lista
  if (id < 0) {
    for (int i = 0; i < MAX_ALARMES; i++) {
      if (!alarmes[i].usado) { id = i; break; }
    }
    if (id < 0) {
      server.send(409, "text/plain", "Limite de alarmes atingido. Exclua algum para criar outro.");
      return;
    }
  } else if (id >= MAX_ALARMES || !alarmes[id].usado) {
    server.send(404, "text/plain", "Alarme não encontrado.");
    return;
  }

  // Guarda o alarme
  alarmes[id].usado = 1;
  alarmes[id].tipo = tipo;
  alarmes[id].hora = hora;
  alarmes[id].minuto = minuto;
  alarmes[id].intervalo = intervalo;
  for (int i = 0; i < 7; i++) {
    alarmes[id].dias[i] = (tipo == DIAS && dias[i] == '1') ? '1' : '0';
  }
  alarmes[id].dias[7] = '\0';
  copiarNome(alarmes[id].nome, nome);

  salvarAlarmes();
  server.send(200, "application/json", "{\"ok\":true}");
}

// Apaga um alarme
void siteExcluir() {
  int id = server.hasArg("id") ? server.arg("id").toInt() : -1;
  if (id < 0 || id >= MAX_ALARMES || !alarmes[id].usado) {
    server.send(404, "text/plain", "Alarme não encontrado.");
    return;
  }
  memset(&alarmes[id], 0, sizeof(Alarme));
  salvarAlarmes();
  server.send(200, "application/json", "{\"ok\":true}");
}

// ================== 9. SETUP (roda uma vez ao ligar) ==================

void setup() {
  // Configura os pinos
  pinMode(BUZZER1, OUTPUT);
  pinMode(BUZZER2, OUTPUT);
  pinMode(BUZZER3, OUTPUT);
  pinMode(TAMPA, INPUT_PULLUP);   // pull-up: o pino le HIGH quando o fim de curso esta aberto
  pinMode(BOTAO, INPUT_PULLUP);
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);    // luz do display sempre acesa

  // Liga o display
  SPI.begin(TFT_SCK, -1, TFT_SDA, TFT_CS);
  tft.initR(INITR_BLACKTAB);      // se a imagem sair estranha, tente INITR_GREENTAB ou INITR_REDTAB
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(5, 5);
  tft.print("Conectando no Wi-Fi...");

  // Abre a memoria e carrega os alarmes salvos
  memoria.begin("alarmes", false);
  carregarAlarmes();

  // Conecta no Wi-Fi. Se nao houver rede salva, cria a rede "ClockMédio" para configurar.
  WiFiManager wm;
  wm.setAPCallback(telaConfigurarWifi);
  wm.setConnectTimeout(20);          // segundos tentando a rede salva
  wm.setConfigPortalTimeout(300);    // segundos esperando a configuracao pelo celular
  if (!wm.autoConnect("ClockMédio")) {
    ESP.restart();                   // nao conseguiu: reinicia e tenta de novo
  }
  WiFi.setSleep(false);              // desliga a economia de energia do Wi-Fi: o site responde melhor

  // Pega a hora na internet (fuso de Brasilia/Fortaleza: UTC-3)
  configTzTime("<-03>3", "pool.ntp.org", "time.google.com");
  struct tm agora;
  unsigned long inicio = millis();
  while (!getLocalTime(&agora, 500) && millis() - inicio < 15000) {
    // espera ate 15 segundos
  }

  // Liga o site
  MDNS.begin("clockmedio");              // tambem abre em http://clockmedio.local
  server.on("/", HTTP_GET, siteInicio);
  server.on("/api/alarmes", HTTP_GET, siteListar);
  server.on("/api/salvar", HTTP_POST, siteSalvar);
  server.on("/api/excluir", HTTP_POST, siteExcluir);
  server.begin();

  telaRelogio();
}

// ================== 10. LOOP (repete para sempre) ==================

void loop() {
  verificarBotaoWifi();
  server.handleClient();          // atende o celular
  if (!horaValida()) return;      // espera a hora ser acertada

  time_t agora = time(nullptr);
  struct tm t;
  localtime_r(&agora, &t);

  if (!tocando) {
    // --- Modo normal: mostra a hora e confere os alarmes uma vez por minuto ---
    mostrarHora(t);

    int minutoDoDia = t.tm_hour * 60 + t.tm_min;
    if (minutoDoDia != ultimoMinuto) {
      ultimoMinuto = minutoDoDia;
      int diaSemana = (t.tm_wday + 6) % 7;   // tm_wday: 0 = domingo. Aqui: 0 = segunda ... 6 = domingo

      for (int i = 0; i < MAX_ALARMES; i++) {
        if (alarmes[i].usado && alarmeEstaNaHora(alarmes[i], minutoDoDia, diaSemana)) {
          tocando = true;
          tampaJaFechou = !tampaAberta();   // se a tampa ja esta aberta, ainda nao "fechou"
          telaAlarme(alarmes[i].nome);
          dicaTampa(!tampaJaFechou);        // pede para fechar e abrir de novo, se for o caso
          break;
        }
      }
    }
    delay(50);

  } else {
    // --- Alarme tocando: apita ate a tampa ser aberta ---
    // (se a tampa ja estava aberta, ela precisa fechar e abrir de novo)
    bipe(300);
    delay(200);
    server.handleClient();

    if (!tampaAberta()) {
      if (!tampaJaFechou) {         // a tampa estava aberta e acabou de fechar
        tampaJaFechou = true;
        dicaTampa(false);           // some com a dica
      }
    } else if (tampaJaFechou) {     // estava fechada e agora abriu: tomou o remedio
      tocando = false;
      telaRelogio();
    }
  }
}

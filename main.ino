/*
 * ALARME DE REMEDIO - COM SELECAO DE DIAS E REPETICAO
 * ESP32-S3 + display 1.77" (ST7735) + 3 buzzers + fim de curso + Botao de Reset Wi-Fi
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <time.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// ================== 1. CONFIGURACOES ==================

#define MAX_ALARMES 10   // quantos alarmes cabem

// ================== 2. PINOS ==================

// Display
#define TFT_SCK  12
#define TFT_SDA  11
#define TFT_CS   10
#define TFT_RS    9
#define TFT_RST   8
#define TFT_LED   7

// Buzzers e fim de curso
#define BUZZER1 38
#define BUZZER2 39
#define BUZZER3 40
#define TAMPA   21

// Botao para resetar o Wi-Fi
#define BOTAO_WIFI 0

// ================== 3. VARIAVEIS ==================

Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_RS, TFT_RST);
WebServer server(80);
Preferences memoria;

// Cada alarme guarda estas informacoes
struct Alarme {
  uint8_t usado;       // 1 = esta posicao da lista tem um alarme
  uint8_t hora;        // 0 a 23
  uint8_t minuto;      // 0 a 59
  uint8_t repetir;     // 1 = repete, 0 = toca apenas uma vez
  uint8_t diasSemana;  // Bitmask dos dias (Bit 0=Seg, Bit 1=Ter, ..., Bit 6=Dom)
  char nome[33];       // nome do remedio
  uint32_t ancora;     // timestamp de referencia
};

Alarme alarmes[MAX_ALARMES];   

bool tocando = false;          
uint32_t ultimaChecagem = 0;   
int ultimoSegundo = -1;        
bool tampaJaFechou = false;    

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

// ================== 5. FUNCOES: HORA E CALCULO DOS ALARMES ==================

bool horaValida() {
  return time(nullptr) > 1700000000UL;
}

// Calcula o proximo disparo considerando os dias da semana selecionados
uint32_t proximoDisparo(const Alarme& a, uint32_t instante) {
  time_t t_instante = instante;
  struct tm tm_atual;
  localtime_r(&t_instante, &tm_atual);

  // Se nao repete, a ancora e o momento exato em que foi agendado
  if (!a.repetir) {
    if (instante <= a.ancora) return a.ancora;
    return 0xFFFFFFFFUL; // Ja passou e nao repete (invalido/expirado)
  }

  // Se repete todos os dias (nenhum dia especifico marcado, ou todos marcados)
  uint8_t dias = a.diasSemana == 0 ? 0x7F : a.diasSemana;

  // Testa os proximos 8 dias para achar o primeiro dia valido
  for (int d = 0; d < 8; d++) {
    struct tm tm_teste = tm_atual;
    tm_teste.tm_mday += d;
    tm_teste.tm_hour = a.hora;
    tm_teste.tm_min = a.minuto;
    tm_teste.tm_sec = 0;
    tm_teste.tm_isdst = -1;
    
    time_t tempoAlvo = mktime(&tm_teste);
    if (tempoAlvo < instante) continue;

    // Converte tm_wday (0=Domingo, 1=Segunda...) para o nosso formato (0=Segunda ... 6=Domingo)
    int wday = tm_teste.tm_wday; 
    int nossoDia = (wday == 0) ? 6 : (wday - 1);

    if (dias & (1 << nossoDia)) {
      return (uint32_t)tempoAlvo;
    }
  }
  return a.ancora;
}

String textoProximo(uint32_t disparo) {
  if (disparo == 0xFFFFFFFFUL) return String("Expirado");
  time_t d = disparo;
  time_t agora = time(nullptr);
  struct tm td, ta;
  localtime_r(&d, &td);
  localtime_r(&agora, &ta);
  char texto[20];
  if (td.tm_yday == ta.tm_yday && td.tm_year == ta.tm_year) {
    snprintf(texto, sizeof(texto), "%02d:%02d", td.tm_hour, td.tm_min);
  } else {
    snprintf(texto, sizeof(texto), "%02d/%02d %02d:%02d", td.tm_mday, td.tm_mon + 1, td.tm_hour, td.tm_min);
  }
  return String(texto);
}

// ================== 6. FUNCOES: DISPLAY ==================

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

void telaAlarme(const char* nome) {
  tft.fillScreen(ST77XX_RED);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE, ST77XX_RED);
  tft.setCursor(30, 10);
  tft.print("TOME O");
  tft.setCursor(30, 30);
  tft.print("REMEDIO!");

  tft.setTextColor(ST77XX_YELLOW, ST77XX_RED);
  tft.setTextSize(strlen(nome) * 12 <= 150 ? 2 : 1);
  tft.setCursor(5, 75);
  tft.print(nome);
}

void dicaTampa(bool mostrar) {
  tft.fillRect(0, 105, tft.width(), 23, ST77XX_RED);
  if (mostrar) {
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE, ST77XX_RED);
    tft.setCursor(5, 112);
    tft.print("Feche e abra a tampa");
  }
}

void telaConfigurarWifi(WiFiManager* wm) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(5, 5);
  tft.print("CONFIGURAR WI-FI");
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(5, 25);
  tft.print("1) No celular, conecte");
  tft.setCursor(5, 36);
  tft.print("na rede:");
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(5, 50);
  tft.print("AlarmeRemedio");
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(5, 80);
  tft.print("2) Abra no navegador:");
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(5, 95);
  tft.print("192.168.4.1");
}

void mostrarHora() {
  time_t agora = time(nullptr);
  struct tm t;
  localtime_r(&agora, &t);
  if (t.tm_sec == ultimoSegundo) return;
  ultimoSegundo = t.tm_sec;

  char texto[9];
  sprintf(texto, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  tft.setCursor(8, 50);
  tft.print(texto);
}

// ================== 7. FUNCOES: BUZZER E TAMPA ==================

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

bool tampaAberta() {
  return digitalRead(TAMPA) == HIGH;
}

// ================== 8. FUNCOES: SITE ==================

const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Alarme de Remédio</title>
<style>
body{font-family:system-ui,sans-serif;background:#10141a;color:#e8edf2;margin:0 auto;padding:16px;max-width:480px}
h1{font-size:22px;margin:0 0 16px}
h2{font-size:18px;margin:0 0 12px}
.card{background:#1b222c;border-radius:12px;padding:14px;margin-bottom:10px}
.hora{font-size:34px;font-weight:700}
.nome{font-size:18px;font-weight:600;margin-bottom:2px;word-break:break-word}
.info{color:#9fb0c3;font-size:14px;margin:4px 0 12px}
.linha{display:flex;gap:8px}
button{flex:1;padding:12px;border:0;border-radius:8px;font-size:16px;background:#2f81f7;color:#fff}
button.sec{background:#3a4452}
button.perigo{background:#d64545}
form{background:#1b222c;border-radius:12px;padding:14px;margin-top:16px}
label{display:block;margin-bottom:12px;font-size:14px;color:#9fb0c3}
input[type="text"], input[type="time"]{display:block;width:100%;box-sizing:border-box;margin-top:6px;padding:12px;font-size:18px;border-radius:8px;border:1px solid #3a4452;background:#10141a;color:#e8edf2}
.dias-container{display:flex;gap:4px;margin-top:6px;margin-bottom:12px}
.dias-container label{flex:1;background:#10141a;border:1px solid #3a4452;padding:8px 4px;text-align:center;border-radius:6px;font-size:12px;cursor:pointer;margin-bottom:0;color:#e8edf2}
.dias-container input{display:none}
.dias-container input:checked + span{color:#2f81f7;font-weight:bold}
.vazio{color:#9fb0c3}
</style>
</head>
<body>
<h1>Alarmes de remédio</h1>
<div id="lista"></div>

<form id="form">
  <h2 id="tituloForm">Novo alarme</h2>
  <label>Nome do remédio (opcional)
    <input type="text" id="nome" maxlength="24" placeholder="Ex.: Losartana 50 mg">
  </label>
  <label>Horário
    <input type="time" id="hora" required>
  </label>
  
  <label style="display:flex;align-items:center;gap:10px;cursor:pointer;">
    <input type="checkbox" id="repetir" checked style="width:20px;height:20px;" onchange="toggleDias(this.checked)"> Repetir alarme
  </label>

  <div id="blocoDias">
    <label>Dias da semana (vazio = todos os dias)</label>
    <div class="dias-container">
      <label><input type="checkbox" name="dia" value="0"><span>Seg</span></label>
      <label><input type="checkbox" name="dia" value="1"><span>Ter</span></label>
      <label><input type="checkbox" name="dia" value="2"><span>Qua</span></label>
      <label><input type="checkbox" name="dia" value="3"><span>Qui</span></label>
      <label><input type="checkbox" name="dia" value="4"><span>Sex</span></label>
      <label><input type="checkbox" name="dia" value="5"><span>Sáb</span></label>
      <label><input type="checkbox" name="dia" value="6"><span>Dom</span></label>
    </div>
  </div>

  <div class="linha">
    <button type="submit" id="btnSalvar">Adicionar</button>
    <button type="button" id="btnCancelar" class="sec" hidden>Cancelar</button>
  </div>
</form>

<script>
let editando = -1;
const p = n => String(n).padStart(2, '0');

function toggleDias(ativo) {
  document.getElementById('blocoDias').style.display = ativo ? 'block' : 'none';
}

function btn(texto, classe, fn) {
  const b = document.createElement('button');
  b.textContent = texto;
  b.className = classe;
  b.type = 'button';
  b.onclick = fn;
  return b;
}

function render(lista) {
  const el = document.getElementById('lista');
  el.innerHTML = '';
  if (!lista.length) {
    el.innerHTML = '<p class="vazio">Nenhum alarme criado.</p>';
    return;
  }
  lista.sort((x, y) => (x.hora * 60 + x.minuto) - (y.hora * 60 + y.minuto));
  lista.forEach(a => {
    const card = document.createElement('div');
    card.className = 'card';
    
    let infoStr = '';
    if (!a.repetir) {
      infoStr = 'Toca apenas uma vez<br>Próximo: ' + a.proximo;
    } else {
      const nomesDias = ['Seg', 'Ter', 'Qua', 'Qui', 'Sex', 'Sáb', 'Dom'];
      let dSel = [];
      for(let i=0; i<7; i++) {
        if(a.dias & (1 << i)) dSel.push(nomesDias[i]);
      }
      let txtDias = dSel.length === 0 || dSel.length === 7 ? 'Todos os dias' : dSel.join(', ');
      infoStr = txtDias + '<br>Próximo: ' + a.proximo;
    }

    card.innerHTML = '<div class="hora">' + p(a.hora) + ':' + p(a.minuto) + '</div>' +
                     '<div class="info">' + infoStr + '</div>';
    if (a.nome) {
      const n = document.createElement('div');
      n.className = 'nome';
      n.textContent = a.nome;
      card.insertBefore(n, card.firstChild);
    }
    const acoes = document.createElement('div');
    acoes.className = 'linha';
    acoes.appendChild(btn('Editar', 'sec', () => editar(a)));
    acoes.appendChild(btn('Excluir', 'perigo', () => excluir(a)));
    card.appendChild(acoes);
    el.appendChild(card);
  });
}

async function carregar() {
  try {
    const r = await fetch('/api/alarmes');
    render(await r.json());
  } catch (e) {}
}

function editar(a) {
  editando = a.id;
  document.getElementById('nome').value = a.nome || '';
  document.getElementById('hora').value = p(a.hora) + ':' + p(a.minuto);
  document.getElementById('repetir').checked = a.repetir;
  toggleDias(a.repetir);
  
  const checkboxes = document.querySelectorAll('input[name="dia"]');
  checkboxes.forEach((cb, i) => {
    cb.checked = (a.dias & (1 << i)) !== 0;
  });

  document.getElementById('tituloForm').textContent = 'Editar alarme';
  document.getElementById('btnSalvar').textContent = 'Salvar alterações';
  document.getElementById('btnCancelar').hidden = false;
  document.getElementById('form').scrollIntoView({behavior: 'smooth'});
}

function cancelarEdicao() {
  editando = -1;
  document.getElementById('form').reset();
  toggleDias(true);
  document.getElementById('tituloForm').textContent = 'Novo alarme';
  document.getElementById('btnSalvar').textContent = 'Adicionar';
  document.getElementById('btnCancelar').hidden = true;
}

async function excluir(a) {
  if (!confirm('Excluir o alarme das ' + p(a.hora) + ':' + p(a.minuto) + '?')) return;
  try {
    const r = await fetch('/api/excluir', {method: 'POST', body: new URLSearchParams({id: a.id})});
    if (!r.ok) alert(await r.text());
    if (editando === a.id) cancelarEdicao();
    carregar();
  } catch (e) {
    alert('Sem conexão com o dispositivo.');
  }
}

document.getElementById('btnCancelar').onclick = cancelarEdicao;

document.getElementById('form').addEventListener('submit', async e => {
  e.preventDefault();
  const partes = document.getElementById('hora').value.split(':');
  const repetir = document.getElementById('repetir').checked ? 1 : 0;
  
  let diasBitmask = 0;
  const checkboxes = document.querySelectorAll('input[name="dia"]:checked');
  checkboxes.forEach(cb => {
    diasBitmask |= (1 << parseInt(cb.value));
  });

  const nome = document.getElementById('nome').value.normalize('NFD').replace(/[\u0300-\u036f]/g, '');
  const corpo = new URLSearchParams({
    id: editando, 
    hora: partes[0], 
    minuto: partes[1], 
    repetir: repetir, 
    dias: diasBitmask, 
    nome: nome
  });

  try {
    const r = await fetch('/api/salvar', {method: 'POST', body: corpo});
    if (!r.ok) { alert(await r.text()); return; }
    cancelarEdicao();
    carregar();
  } catch (err) {
    alert('Sem conexão com o dispositivo.');
  }
});

carregar();
setInterval(carregar, 30000);
</script>
</body>
</html>
)rawliteral";

void siteInicio() {
  server.send(200, "text/html; charset=utf-8", PAGINA);
}

void siteListar() {
  uint32_t agora = (uint32_t)time(nullptr);
  String resposta = "[";
  bool primeiro = true;
  for (int i = 0; i < MAX_ALARMES; i++) {
    if (!alarmes[i].usado) continue;
    if (!primeiro) resposta += ",";
    primeiro = false;
    uint32_t disp = proximoDisparo(alarmes[i], agora);
    String proximo = horaValida() ? textoProximo(disp) : String("--");
    
    resposta += "{\"id\":" + String(i) +
                ",\"hora\":" + String(alarmes[i].hora) +
                ",\"minuto\":" + String(alarmes[i].minuto) +
                ",\"repetir\":" + String(alarmes[i].repetir) +
                ",\"dias\":" + String(alarmes[i].diasSemana) +
                ",\"nome\":\"" + String(alarmes[i].nome) + "\"" +
                ",\"proximo\":\"" + proximo + "\"}";
  }
  resposta += "]";
  server.send(200, "application/json", resposta);
}

void siteSalvar() {
  if (!server.hasArg("hora") || !server.hasArg("minuto")) {
    server.send(400, "text/plain", "Dados incompletos.");
    return;
  }
  int id = server.hasArg("id") ? server.arg("id").toInt() : -1;
  int hora = server.arg("hora").toInt();
  int minuto = server.arg("minuto").toInt();
  int repetir = server.hasArg("repetir") ? server.arg("repetir").toInt() : 1;
  uint8_t dias = server.hasArg("dias") ? server.arg("dias").toInt() : 0;
  String nome = server.hasArg("nome") ? server.arg("nome") : String("");

  if (hora < 0 || hora > 23 || minuto < 0 || minuto > 59) {
    server.send(400, "text/plain", "Horário inválido.");
    return;
  }
  if (!horaValida()) {
    server.send(503, "text/plain", "A hora ainda não foi sincronizada.");
    return;
  }

  if (id < 0) {   
    for (int i = 0; i < MAX_ALARMES; i++) {
      if (!alarmes[i].usado) { id = i; break; }
    }
    if (id < 0) {
      server.send(409, "text/plain", "Limite de alarmes atingido.");
      return;
    }
  } else if (id >= MAX_ALARMES || !alarmes[id].usado) {
    server.send(404, "text/plain", "Alarme não encontrado.");
    return;
  }

  alarmes[id].usado = 1;
  alarmes[id].hora = hora;
  alarmes[id].minuto = minuto;
  alarmes[id].repetir = repetir;
  alarmes[id].diasSemana = dias;
  copiarNome(alarmes[id].nome, nome);
  
  // Define o timestamp base (ancora)
  time_t agora = time(nullptr);
  struct tm t;
  localtime_r(&agora, &t);
  t.tm_hour = hora;
  t.tm_min = minuto;
  t.tm_sec = 0;
  t.tm_isdst = -1;
  time_t alvo = mktime(&t);
  if (alvo <= agora && !repetir) alvo += 86400; // se ja passou hoje e e unico, joga para amanha para calcular direito
  alarmes[id].ancora = (uint32_t)alvo;

  salvarAlarmes();
  server.send(200, "application/json", "{\"ok\":true}");
}

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

// ================== 9. SETUP ==================

void setup() {
  pinMode(BUZZER1, OUTPUT);
  pinMode(BUZZER2, OUTPUT);
  pinMode(BUZZER3, OUTPUT);
  pinMode(TAMPA, INPUT_PULLUP);
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);
  pinMode(BOTAO_WIFI, INPUT_PULLUP);

  SPI.begin(TFT_SCK, -1, TFT_SDA, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(5, 5);

  memoria.begin("alarmes", false);
  carregarAlarmes();

  WiFiManager wm;
  if (digitalRead(BOTAO_WIFI) == LOW) {
    tft.print("Resetando Wi-Fi...");
    delay(1000);
    wm.resetSettings();
  }

  wm.setAPCallback(telaConfigurarWifi);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(300);

  tft.setCursor(5, 5);
  tft.print("Conectando no Wi-Fi...");

  if (!wm.autoConnect("AlarmeRemedio")) {
    ESP.restart();
  }

  configTzTime("<-03>3", "pool.ntp.org", "time.google.com");
  struct tm agora;
  unsigned long inicio = millis();
  while (!getLocalTime(&agora, 500) && millis() - inicio < 15000) {}

  MDNS.begin("alarme");
  server.on("/", HTTP_GET, siteInicio);
  server.on("/api/alarmes", HTTP_GET, siteListar);
  server.on("/api/salvar", HTTP_POST, siteSalvar);
  server.on("/api/excluir", HTTP_POST, siteExcluir);
  server.begin();

  telaRelogio();
}

// ================== 10. LOOP ==================

void loop() {
  if (digitalRead(BOTAO_WIFI) == LOW) {
    unsigned long tempoInicioPress = millis();
    while (digitalRead(BOTAO_WIFI) == LOW) {
      if (millis() - tempoInicioPress > 3000) {
        tft.fillScreen(ST77XX_BLACK);
        tft.setCursor(5, 50);
        tft.setTextSize(1);
        tft.setTextColor(ST77XX_YELLOW);
        tft.print("Resetando Wi-Fi...");
        
        WiFiManager wm;
        wm.resetSettings();
        delay(1000);
        ESP.restart();
      }
      delay(100);
    }
  }

  server.handleClient();
  if (!horaValida()) return;

  uint32_t agora = (uint32_t)time(nullptr);
  if (ultimaChecagem == 0) ultimaChecagem = agora;

  if (!tocando) {
    mostrarHora();

    if (agora != ultimaChecagem) {
      for (int i = 0; i < MAX_ALARMES; i++) {
        if (!alarmes[i].usado) continue;
        
        uint32_t disp = proximoDisparo(alarmes[i], ultimaChecagem + 1);
        if (disp != 0xFFFFFFFFUL && disp <= agora) {
          tocando = true;
          tampaJaFechou = !tampaAberta();
          telaAlarme(alarmes[i].nome);
          dicaTampa(!tampaJaFechou);

          // Se for um alarme que toca apenas uma vez, desativa/exclui ele da memoria para nao tocar de novo
          if (!alarmes[i].repetir) {
            memset(&alarmes[i], 0, sizeof(Alarme));
            salvarAlarmes();
          }
          break;
        }
      }
      ultimaChecagem = agora;
    }
    delay(50);

  } else {
    bipe(300);
    delay(200);
    server.handleClient();

    if (!tampaAberta()) {
      if (!tampaJaFechou) {
        tampaJaFechou = true;
        dicaTampa(false);
      }
    } else if (tampaJaFechou) {
      tocando = false;
      telaRelogio();
    }
  }
}

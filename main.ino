/*
 * ALARME DE REMEDIO - COMPLETO (versao corrigida)
 * ESP32-S3 + display 1.77" (ST7735) + 3 buzzers + fim de curso
 *
 * Luz do display:
 *  - fica acesa por 2 minutos depois de ligar o aparelho;
 *  - acende por 30 segundos quando voce da um toque curto no botao BOOT;
 *  - acende 3 minutos antes de um alarme e fica acesa ate 3 minutos depois dele;
 *  - fica sempre acesa enquanto o alarme toca.
 *
 * Botao BOOT (GPIO 0): toque curto = acende a tela; segurar 3 segundos = apaga o Wi-Fi salvo.
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

#define MAX_ALARMES 10                    // quantos alarmes cabem
#define LUZ_APOS_LIGAR_MS 120000UL        // luz acesa por 2 minutos depois de ligar
#define LUZ_APOS_TOQUE_MS 30000UL         // luz acesa por 30 s depois de um toque no botao
#define JANELA_LUZ_S 180                  // luz acesa 3 minutos antes e depois dos alarmes

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

// Botao BOOT da placa (GPIO 0)
#define BOTAO 0

// ================== 3. VARIAVEIS ==================

Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_RS, TFT_RST);
WebServer server(80);
Preferences memoria;

struct Alarme {
  uint8_t usado;       // 1 = esta posicao da lista tem um alarme
  uint8_t hora;        // 0 a 23
  uint8_t minuto;      // 0 a 59
  uint8_t repetir;     // 0 = toca uma vez so; 1 = repete
  uint8_t intervalo;   // 0 = usa os dias da semana; 1 a 24 = repete a cada tantas horas
  uint8_t diasSemana;  // bits: Seg=1, Ter=2, Qua=4, Qui=8, Sex=16, Sab=32, Dom=64 (0 = todos)
  char nome[33];       // nome do remedio
  uint32_t ancora;     // primeira vez que toca (segundos desde 1970)
};

Alarme alarmes[MAX_ALARMES];

bool tocando = false;               // true enquanto o alarme esta tocando
uint32_t ultimaChecagem = 0;        // ultimo segundo em que conferimos os alarmes
int ultimoSegundo = -1;             // para so redesenhar a hora quando o segundo muda
bool tampaJaFechou = false;         // true quando a tampa foi vista fechada durante o alarme
bool displayLigado = true;          // estado atual da luz do display
unsigned long luzAcesaAte = 0;      // a luz fica acesa ate este instante (millis)
uint32_t ultimoEventoAlarme = 0;    // ultima vez que um alarme comecou ou parou de tocar

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

// ================== 5. FUNCOES: HORA E CALCULO DOS ALARMES ==================

// So vale depois que a internet acertou o relogio
bool horaValida() {
  return time(nullptr) > 1700000000UL;
}

// Proxima vez que o alarme toca, a partir de um instante.
// Devolve 0xFFFFFFFF se o alarme nao vai tocar mais (alarme de uma vez so que ja passou).
uint32_t proximoDisparo(const Alarme& a, uint32_t instante) {
  // Alarme de uma vez so
  if (!a.repetir) {
    if (instante <= a.ancora) return a.ancora;
    return 0xFFFFFFFFUL;
  }

  // Repete a cada X horas
  if (a.intervalo > 0) {
    uint32_t periodo = a.intervalo * 3600UL;
    if (instante <= a.ancora) return a.ancora;
    uint32_t k = (instante - a.ancora + periodo - 1) / periodo;
    return a.ancora + k * periodo;
  }

  // Repete nos dias da semana escolhidos
  time_t t_instante = instante;
  struct tm tm_atual;
  localtime_r(&t_instante, &tm_atual);

  uint8_t dias = a.diasSemana == 0 ? 0x7F : a.diasSemana;

  for (int d = 0; d < 8; d++) {
    struct tm tm_teste = tm_atual;
    tm_teste.tm_mday += d;
    tm_teste.tm_hour = a.hora;
    tm_teste.tm_min = a.minuto;
    tm_teste.tm_sec = 0;
    tm_teste.tm_isdst = -1;

    time_t tempoAlvo = mktime(&tm_teste);
    if (tempoAlvo < instante) continue;

    int wday = tm_teste.tm_wday;                 // 0 = domingo
    int nossoDia = (wday == 0) ? 6 : (wday - 1); // 0 = segunda ... 6 = domingo

    if (dias & (1 << nossoDia)) {
      return (uint32_t)tempoAlvo;
    }
  }
  return a.ancora;
}

// Escreve o disparo como "08:00" (se for hoje) ou "05/11 08:00" (outro dia)
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
  tft.setTextSize(strlen(nome) * 12 <= 150 ? 2 : 1);
  tft.setCursor(5, 75);
  tft.print(nome);
}

// Dica no pe da tela do alarme: aparece se a tampa ja estava aberta quando o alarme tocou
void dicaTampa(bool mostrar) {
  tft.fillRect(0, 105, tft.width(), 23, ST77XX_RED);
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

// Escreve a hora no display, no formato HH:MM:SS (so quando o segundo muda)
void mostrarHora() {
  if (!displayLigado) return;   // tela apagada: nao gasta tempo desenhando
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

// Decide se a luz do display fica acesa ou apagada
void controlarLuzDisplay(uint32_t agora) {
  // Alarme tocando: luz sempre acesa (a tela do alarme ja esta desenhada)
  if (tocando) {
    if (!displayLigado) {
      digitalWrite(TFT_LED, HIGH);
      displayLigado = true;
    }
    return;
  }

  bool deveLigar = false;

  if ((long)(luzAcesaAte - millis()) > 0) {
    // 2 minutos depois de ligar, ou 30 segundos depois de um toque no botao
    deveLigar = true;
  } else {
    // Falta 3 minutos ou menos para algum alarme?
    for (int i = 0; i < MAX_ALARMES; i++) {
      if (!alarmes[i].usado) continue;
      uint32_t disp = proximoDisparo(alarmes[i], agora);
      if (disp != 0xFFFFFFFFUL && disp >= agora && disp - agora <= JANELA_LUZ_S) {
        deveLigar = true;
        break;
      }
    }
    // Um alarme comecou ou parou de tocar ha menos de 3 minutos?
    if (ultimoEventoAlarme != 0 && agora - ultimoEventoAlarme <= JANELA_LUZ_S) {
      deveLigar = true;
    }
  }

  if (deveLigar && !displayLigado) {
    digitalWrite(TFT_LED, HIGH);
    displayLigado = true;
    telaRelogio();                 // redesenha a tela ao acender
  } else if (!deveLigar && displayLigado) {
    digitalWrite(TFT_LED, LOW);    // apaga a luz de fundo
    displayLigado = false;
  }
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

// Botao BOOT: toque curto acende a tela; segurar 3 segundos apaga o Wi-Fi salvo
void verificarBotao() {
  if (digitalRead(BOTAO) != LOW) return;

  unsigned long inicio = millis();
  while (digitalRead(BOTAO) == LOW) {
    if (millis() - inicio > 3000) {
      digitalWrite(TFT_LED, HIGH);
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
    delay(20);
  }

  if (millis() - inicio >= 50) {   // ignora ruido muito curto
    luzAcesaAte = millis() + LUZ_APOS_TOQUE_MS;
  }
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
input[type="text"], input[type="time"], input[type="number"]{display:block;width:100%;box-sizing:border-box;margin-top:6px;padding:12px;font-size:18px;border-radius:8px;border:1px solid #3a4452;background:#10141a;color:#e8edf2}
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
    <input type="checkbox" id="repetir" checked style="width:20px;height:20px;" onchange="toggleOpcoes(this.checked)"> Repetir alarme
  </label>

  <div id="blocoOpcoes">
    <label>Repetir a cada quantas horas? (opcional)
      <input type="number" id="intervalo" min="1" max="24" inputmode="numeric" placeholder="Deixe em branco para usar dias da semana">
    </label>

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

function toggleOpcoes(ativo) {
  document.getElementById('blocoOpcoes').style.display = ativo ? 'block' : 'none';
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
    } else if (a.intervalo > 0) {
      infoStr = 'A cada ' + a.intervalo + ' h<br>Próximo: ' + a.proximo;
    } else {
      const nomesDias = ['Seg', 'Ter', 'Qua', 'Qui', 'Sex', 'Sáb', 'Dom'];
      let dSel = [];
      for(let i=0; i<7; i++) {
        if(a.dias & (1 << i)) dSel.push(nomesDias[i]);
      }
      let txtDias = dSel.length === 0 || dSel.length === 7 ? 'Todo dia' : dSel.join(', ');
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
  document.getElementById('intervalo').value = a.intervalo > 0 ? a.intervalo : '';
  toggleOpcoes(a.repetir);

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
  toggleOpcoes(true);
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
  const intervalo = document.getElementById('intervalo').value || '0';

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
    intervalo: intervalo,
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

// Entrega a pagina ao celular
void siteInicio() {
  server.send(200, "text/html; charset=utf-8", PAGINA);
}

// Entrega a lista de alarmes (a pagina usa isto para desenhar os cartoes)
void siteListar() {
  uint32_t agora = (uint32_t)time(nullptr);
  String resposta = "[";
  bool primeiro = true;
  for (int i = 0; i < MAX_ALARMES; i++) {
    if (!alarmes[i].usado) continue;
    if (!primeiro) resposta += ",";
    primeiro = false;
    String proximo = horaValida() ? textoProximo(proximoDisparo(alarmes[i], agora)) : String("--");

    resposta += "{\"id\":" + String(i) +
                ",\"hora\":" + String(alarmes[i].hora) +
                ",\"minuto\":" + String(alarmes[i].minuto) +
                ",\"repetir\":" + String(alarmes[i].repetir) +
                ",\"intervalo\":" + String(alarmes[i].intervalo) +
                ",\"dias\":" + String(alarmes[i].diasSemana) +
                ",\"nome\":\"" + String(alarmes[i].nome) + "\"" +
                ",\"proximo\":\"" + proximo + "\"}";
  }
  resposta += "]";
  server.send(200, "application/json", resposta);
}

// Cria um alarme novo (id = -1) ou edita um existente
void siteSalvar() {
  if (!server.hasArg("hora") || !server.hasArg("minuto")) {
    server.send(400, "text/plain", "Dados incompletos.");
    return;
  }
  int id = server.hasArg("id") ? server.arg("id").toInt() : -1;
  int hora = server.arg("hora").toInt();
  int minuto = server.arg("minuto").toInt();
  int repetir = server.hasArg("repetir") ? server.arg("repetir").toInt() : 1;
  int intervalo = server.hasArg("intervalo") ? server.arg("intervalo").toInt() : 0;
  uint8_t dias = server.hasArg("dias") ? server.arg("dias").toInt() : 0;
  String nome = server.hasArg("nome") ? server.arg("nome") : String("");

  if (hora < 0 || hora > 23 || minuto < 0 || minuto > 59 || intervalo < 0 || intervalo > 24) {
    server.send(400, "text/plain", "Valores inválidos.");
    return;
  }
  if (!horaValida()) {
    server.send(503, "text/plain", "A hora ainda não foi sincronizada.");
    return;
  }

  if (id < 0) {   // alarme novo: procura uma posicao livre na lista
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
  alarmes[id].intervalo = intervalo;
  alarmes[id].diasSemana = dias;
  copiarNome(alarmes[id].nome, nome);

  // Primeira vez que o alarme toca
  time_t agora = time(nullptr);
  struct tm t;
  localtime_r(&agora, &t);
  t.tm_hour = hora;
  t.tm_min = minuto;
  t.tm_sec = 0;
  t.tm_isdst = -1;
  time_t alvo = mktime(&t);
  if (alvo <= agora && (intervalo > 0 || !repetir)) {
    alvo += (intervalo > 0 ? intervalo * 3600UL : 86400UL);
  }
  alarmes[id].ancora = (uint32_t)alvo;

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
  pinMode(BUZZER1, OUTPUT);
  pinMode(BUZZER2, OUTPUT);
  pinMode(BUZZER3, OUTPUT);
  pinMode(TAMPA, INPUT_PULLUP);
  pinMode(BOTAO, INPUT_PULLUP);
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);   // comeca ligado para mostrar o andamento
  displayLigado = true;

  SPI.begin(TFT_SCK, -1, TFT_SDA, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(5, 5);
  tft.print("Conectando no Wi-Fi...");

  memoria.begin("alarmes", false);
  carregarAlarmes();

  // Conecta no Wi-Fi. Se nao houver rede salva, cria a rede "AlarmeRemedio" para configurar.
  WiFiManager wm;
  wm.setAPCallback(telaConfigurarWifi);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(300);
  if (!wm.autoConnect("AlarmeRemedio")) {
    ESP.restart();
  }
  WiFi.setSleep(false);   // desliga a economia de energia do Wi-Fi: o site responde melhor

  // Pega a hora na internet (fuso de Brasilia/Fortaleza: UTC-3)
  configTzTime("<-03>3", "pool.ntp.org", "time.google.com");
  struct tm agora;
  unsigned long inicio = millis();
  while (!getLocalTime(&agora, 500) && millis() - inicio < 15000) {}

  // Liga o site
  MDNS.begin("alarme");
  server.on("/", HTTP_GET, siteInicio);
  server.on("/api/alarmes", HTTP_GET, siteListar);
  server.on("/api/salvar", HTTP_POST, siteSalvar);
  server.on("/api/excluir", HTTP_POST, siteExcluir);
  server.begin();

  luzAcesaAte = millis() + LUZ_APOS_LIGAR_MS;   // luz acesa por 2 minutos para voce ver o endereco
  telaRelogio();
}

// ================== 10. LOOP (repete para sempre) ==================

void loop() {
  verificarBotao();
  server.handleClient();
  if (!horaValida()) return;

  uint32_t agora = (uint32_t)time(nullptr);
  controlarLuzDisplay(agora);

  if (ultimaChecagem == 0) ultimaChecagem = agora;

  if (!tocando) {
    // --- Modo normal: mostra a hora e confere se algum alarme disparou ---
    mostrarHora();

    if (agora != ultimaChecagem) {
      for (int i = 0; i < MAX_ALARMES; i++) {
        if (!alarmes[i].usado) continue;

        uint32_t disp = proximoDisparo(alarmes[i], ultimaChecagem + 1);
        if (disp != 0xFFFFFFFFUL && disp <= agora) {
          tocando = true;
          tampaJaFechou = !tampaAberta();
          ultimoEventoAlarme = agora;

          // Acende a tela na hora do alarme
          digitalWrite(TFT_LED, HIGH);
          displayLigado = true;

          telaAlarme(alarmes[i].nome);
          dicaTampa(!tampaJaFechou);

          if (!alarmes[i].repetir) {   // alarme de uma vez so: apaga da lista
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
    // --- Alarme tocando: apita ate a tampa ser aberta ---
    // (se a tampa ja estava aberta, ela precisa fechar e abrir de novo)
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
      ultimoEventoAlarme = (uint32_t)time(nullptr);
      telaRelogio();
    }
  }
}

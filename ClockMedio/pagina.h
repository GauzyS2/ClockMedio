// pagina.h - a pagina do site (HTML, CSS e JavaScript).
// O ESP32 entrega este texto ao celular quando voce abre o endereco dele no navegador.

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
input[type="text"],input[type="time"],select{display:block;width:100%;box-sizing:border-box;margin-top:6px;padding:12px;font-size:18px;border-radius:8px;border:1px solid #3a4452;background:#10141a;color:#e8edf2}
.dias-container{display:flex;gap:4px;margin-bottom:12px}
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

  <label>Nome do remédio (opcional, sem acentos)
    <input type="text" id="nome" maxlength="24" placeholder="Ex.: Losartana 50 mg">
  </label>

  <label>Horário
    <input type="time" id="hora" required>
  </label>

  <label>Tipo de alarme
    <select id="tipo" onchange="mostrarTipo()">
      <option value="0">Todo dia, ou a cada X horas</option>
      <option value="1">Só em dias da semana</option>
    </select>
  </label>

  <div id="blocoPadrao">
    <label>Repetição
      <select id="intervalo">
        <option value="24">Todo dia, no mesmo horário</option>
        <option value="12">A cada 12 horas</option>
        <option value="8">A cada 8 horas</option>
        <option value="6">A cada 6 horas</option>
        <option value="4">A cada 4 horas</option>
        <option value="3">A cada 3 horas</option>
        <option value="2">A cada 2 horas</option>
        <option value="1">A cada 1 hora</option>
      </select>
    </label>
  </div>

  <div id="blocoDias" hidden>
    <label>Dias da semana</label>
    <div class="dias-container">
      <label><input type="checkbox" name="dia"><span>Seg</span></label>
      <label><input type="checkbox" name="dia"><span>Ter</span></label>
      <label><input type="checkbox" name="dia"><span>Qua</span></label>
      <label><input type="checkbox" name="dia"><span>Qui</span></label>
      <label><input type="checkbox" name="dia"><span>Sex</span></label>
      <label><input type="checkbox" name="dia"><span>Sáb</span></label>
      <label><input type="checkbox" name="dia"><span>Dom</span></label>
    </div>
  </div>

  <div class="linha">
    <button type="submit" id="btnSalvar">Adicionar</button>
    <button type="button" id="btnCancelar" class="sec" hidden>Cancelar</button>
  </div>
</form>

<script>
let editando = -1;   // -1 = criando um alarme novo; 0 ou mais = editando esse alarme
const nomesDias = ['Seg', 'Ter', 'Qua', 'Qui', 'Sex', 'Sáb', 'Dom'];
const p = n => String(n).padStart(2, '0');

// Mostra as opcoes do tipo de alarme escolhido
function mostrarTipo() {
  const soDias = document.getElementById('tipo').value === '1';
  document.getElementById('blocoPadrao').hidden = soDias;
  document.getElementById('blocoDias').hidden = !soDias;
}

// Texto que descreve quando o alarme toca
function descricao(a) {
  if (a.tipo === 1) {
    return nomesDias.filter((n, i) => a.dias[i] === '1').join(', ');
  }
  if (a.intervalo === 24) return 'Todo dia';
  return 'A cada ' + a.intervalo + ' h, a partir deste horário';
}

function btn(texto, classe, fn) {
  const b = document.createElement('button');
  b.textContent = texto;
  b.className = classe;
  b.type = 'button';
  b.onclick = fn;
  return b;
}

// Desenha a lista de alarmes
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
    if (a.nome) {
      const n = document.createElement('div');
      n.className = 'nome';
      n.textContent = a.nome;
      card.appendChild(n);
    }
    const h = document.createElement('div');
    h.className = 'hora';
    h.textContent = p(a.hora) + ':' + p(a.minuto);
    card.appendChild(h);
    const info = document.createElement('div');
    info.className = 'info';
    info.textContent = descricao(a);
    card.appendChild(info);
    const acoes = document.createElement('div');
    acoes.className = 'linha';
    acoes.appendChild(btn('Editar', 'sec', () => editar(a)));
    acoes.appendChild(btn('Excluir', 'perigo', () => excluir(a)));
    card.appendChild(acoes);
    el.appendChild(card);
  });
}

// Pede a lista ao ESP32
async function carregar() {
  try {
    const r = await fetch('/api/alarmes');
    render(await r.json());
  } catch (e) {}
}

// Coloca um alarme no formulario para editar
function editar(a) {
  editando = a.id;
  document.getElementById('nome').value = a.nome || '';
  document.getElementById('hora').value = p(a.hora) + ':' + p(a.minuto);
  document.getElementById('tipo').value = a.tipo;
  document.getElementById('intervalo').value = a.intervalo;
  document.querySelectorAll('input[name="dia"]').forEach((cb, i) => {
    cb.checked = a.dias[i] === '1';
  });
  mostrarTipo();
  document.getElementById('tituloForm').textContent = 'Editar alarme';
  document.getElementById('btnSalvar').textContent = 'Salvar alterações';
  document.getElementById('btnCancelar').hidden = false;
  document.getElementById('form').scrollIntoView({behavior: 'smooth'});
}

function cancelarEdicao() {
  editando = -1;
  document.getElementById('form').reset();
  mostrarTipo();
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

// Envia o alarme (novo ou editado) ao ESP32
document.getElementById('form').addEventListener('submit', async e => {
  e.preventDefault();
  const partes = document.getElementById('hora').value.split(':');
  const tipo = document.getElementById('tipo').value;

  // Dias da semana como 7 letras: '1' = dia marcado, '0' = nao marcado (Seg ... Dom)
  let dias = '';
  document.querySelectorAll('input[name="dia"]').forEach(cb => {
    dias += cb.checked ? '1' : '0';
  });
  if (tipo === '1' && !dias.includes('1')) {
    alert('Escolha pelo menos um dia da semana.');
    return;
  }

  // Tira os acentos do nome, porque o display do aparelho nao mostra acentos
  const nome = document.getElementById('nome').value.normalize('NFD').replace(/[\u0300-\u036f]/g, '');

  const corpo = new URLSearchParams({
    id: editando,
    hora: partes[0],
    minuto: partes[1],
    tipo: tipo,
    intervalo: document.getElementById('intervalo').value,
    dias: dias,
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
</script>
</body>
</html>
)rawliteral";
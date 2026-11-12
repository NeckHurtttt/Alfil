'use strict';
const $ = s => document.querySelector(s);
const GLIFO = { 1: '♟', 2: '♞', 3: '♝', 4: '♜', 5: '♛', 6: '♚' };
const LETRA = { 2: 'C', 3: 'A', 4: 'T', 5: 'D', 6: 'R' }; // notación en español
const motor = new Worker('motor.js');
let F = null, sel = -1, ultima = null, girado = false, humano = 1, pensando = false, sans = [], pendiente = null;
const De = m => m & 63, A = m => (m >> 6) & 63, Pr = m => (m >> 12) & 7;
const nombre = s => 'abcdefgh'[s & 7] + (1 + (s >> 3));

let idPedido = 0;
const pedir = msg => new Promise(res => { const id = ++idPedido; pendiente = { id, res }; motor.postMessage({ ...msg, id }); });
motor.onmessage = ({ data }) => {
  if (data.tipo === 'foto' && pendiente && data.id === pendiente.id) { F = data; const r = pendiente.res; pendiente = null; r(data); }
  if (data.tipo === 'jugada' && esperaJugada) { const r = esperaJugada; esperaJugada = null; r(data); }
};
let esperaJugada = null;
const pensar = (ms, prof) => new Promise(res => { esperaJugada = res; motor.postMessage({ tipo: 'pensar', ms, prof }); });

function san(m) {
  const t = F.tablero, p = Math.abs(t[De(m)]), cap = t[A(m)] !== 0 || (p === 1 && (De(m) & 7) !== (A(m) & 7));
  if (p === 6 && Math.abs(De(m) - A(m)) === 2) return A(m) > De(m) ? 'O-O' : 'O-O-O';
  let s = '';
  if (p === 1) s = cap ? 'abcdefgh'[De(m) & 7] : '';
  else {
    s = LETRA[p];
    const otros = F.legales.filter(x => x !== m && A(x) === A(m) && Math.abs(t[De(x)]) === p);
    if (otros.length) s += otros.some(x => (De(x) & 7) === (De(m) & 7)) ? (otros.some(x => (De(x) >> 3) === (De(m) >> 3)) ? nombre(De(m)) : 1 + (De(m) >> 3)) : 'abcdefgh'[De(m) & 7];
  }
  s += (cap ? 'x' : '') + nombre(A(m));
  if (Pr(m)) s += '=' + LETRA[Pr(m)];
  return s;
}

function dibujar() {
  const tab = $('#tablero'); tab.innerHTML = '';
  const destinos = sel >= 0 ? F.legales.filter(m => De(m) === sel).map(A) : [];
  const enJaque = F.estado === 1 || F.estado === 2;
  for (let i = 0; i < 64; i++) {
    const fila = girado ? i >> 3 : 7 - (i >> 3), col = girado ? 7 - (i & 7) : i & 7, s = fila * 8 + col;
    const c = document.createElement('div');
    c.className = 'c ' + ((fila + col) % 2 ? 'cl' : 'os');
    const p = F.tablero[s];
    if (ultima && (s === De(ultima) || s === A(ultima))) c.classList.add('ult');
    if (s === sel) c.classList.add('sel');
    if (destinos.includes(s)) c.classList.add('dest', ...(p ? ['cap'] : []));
    if (enJaque && p === 6 * F.turno) c.classList.add('jaque');
    if (p) c.innerHTML = `<span class="p ${p > 0 ? 'b' : 'n'}">${GLIFO[Math.abs(p)]}</span>`;
    if ((i & 7) === 0) c.insertAdjacentHTML('beforeend', `<span class="coordf">${fila + 1}</span>`);
    if (i >> 3 === 7) c.insertAdjacentHTML('beforeend', `<span class="coord">${'abcdefgh'[col]}</span>`);
    c.onclick = () => clic(s);
    tab.appendChild(c);
  }
  const ev = Math.max(-1000, Math.min(1000, F.eval)) / 100;
  $('#relleno').style.height = (50 + 50 * Math.tanh(ev / 4)) + '%';
  $('#evtxt').textContent = (ev > 0 ? '+' : '') + ev.toFixed(1);
  const txt = { 0: F.turno === humano ? 'Tu turno' : 'Alfil piensa…', 1: '¡Jaque!', 2: F.turno === humano ? 'Jaque mate. Ganó Alfil.' : 'Jaque mate. ¡Ganaste!', 3: 'Tablas por ahogado', 4: 'Tablas: regla de 50 jugadas', 5: 'Tablas por repetición', 6: 'Tablas: material insuficiente' };
  $('#estado').textContent = txt[F.estado];
  $('#jugadas').innerHTML = '';
  for (let i = 0; i < sans.length; i += 2) $('#jugadas').insertAdjacentHTML('beforeend', `<li><span>${sans[i]}</span><span>${sans[i + 1] || ''}</span></li>`);
  $('#jugadas').scrollTop = 1e6;
}

const terminado = () => F.estado >= 2;

async function jugar(m) {
  sans.push(san(m)); ultima = m; sel = -1;
  await pedir({ tipo: 'mover', mv: m });
  if (F.estado === 1) sans[sans.length - 1] += '+';
  if (F.estado === 2) sans[sans.length - 1] += '#';
  dibujar();
}

async function turnoMaquina() {
  if (terminado() || F.turno === humano) return;
  pensando = true; dibujar();
  const [ms, prof] = $('#nivel').value.split(',').map(Number);
  const r = await pensar(ms, prof);
  $('#info').textContent = `profundidad ${r.prof} · ${Math.round(r.nodos).toLocaleString('es')} nodos · ${Math.round(r.ms)} ms · ${Math.round(r.nodos / Math.max(1, r.ms))} k nodos/s`;
  pensando = false;
  if (r.mv) await jugar(r.mv);
}

async function clic(s) {
  if (pensando || terminado() || F.turno !== humano) return;
  const p = F.tablero[s];
  if (sel >= 0) {
    const ops = F.legales.filter(m => De(m) === sel && A(m) === s);
    if (ops.length) {
      let m = ops[0];
      if (ops.length > 1) m = await elegirPromo(ops);
      await jugar(m); await turnoMaquina(); return;
    }
  }
  sel = p && Math.sign(p) === humano ? s : -1;
  dibujar();
}

function elegirPromo(ops) {
  return new Promise(res => {
    const d = $('#promo'); d.innerHTML = ''; d.hidden = false;
    [5, 4, 3, 2].forEach(t => {
      const b = document.createElement('button'); b.innerHTML = `<span class="p ${humano > 0 ? 'b' : 'n'}">${GLIFO[t]}</span>`;
      b.onclick = () => { d.hidden = true; res(ops.find(m => Pr(m) === t)); };
      d.appendChild(b);
    });
  });
}

async function nuevo() {
  if (pensando) return;
  humano = +$('#color').value; girado = humano < 0; sans = []; ultima = null; sel = -1;
  await pedir({ tipo: 'nuevo' }); dibujar(); turnoMaquina();
}

$('#nuevo').onclick = nuevo;
$('#color').onchange = nuevo;
$('#girar').onclick = () => { girado = !girado; dibujar(); };
$('#atras').onclick = async () => {
  if (pensando || !sans.length) return;
  const n = F.turno === humano ? 2 : 1, k = Math.min(n, sans.length);
  sans.splice(-k); ultima = null; sel = -1;
  await pedir({ tipo: 'deshacer', n: k }); dibujar();
};
$('#pista').onclick = async () => {
  if (pensando || terminado() || F.turno !== humano) return;
  pensando = true; $('#estado').textContent = 'Buscando una pista…';
  const r = await pensar(600, 32); pensando = false;
  sel = De(r.mv); dibujar(); $('#estado').textContent = `Pista: ${san(r.mv)}`;
};

pedir({ tipo: 'nuevo' }).then(dibujar);

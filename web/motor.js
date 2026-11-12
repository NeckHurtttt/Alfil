// Web Worker: el motor corre aparte para que la página no se congele mientras piensa.
let M = null;
const listo = fetch('alfil.wasm').then(r => r.arrayBuffer())
  .then(b => WebAssembly.instantiate(b, { env: { ahora: () => performance.now() } }))
  .then(r => { M = r.instance.exports; M.reiniciar(); });

function foto() {
  const tablero = Array.from({ length: 64 }, (_, s) => M.pieza(s));
  const n = M.legales();
  const legales = Array.from(new Int32Array(M.memory.buffer, M.buffer(), n));
  return { tablero, turno: M.turno(), legales, estado: M.estado(), eval: M.evaluacion() };
}

onmessage = async ({ data: d }) => {
  await listo;
  if (d.tipo === 'nuevo') M.reiniciar();
  else if (d.tipo === 'mover') M.mover(d.mv);
  else if (d.tipo === 'deshacer') { for (let i = 0; i < d.n; i++) M.deshacer(); }
  else if (d.tipo === 'pensar') {
    const t = performance.now();
    const mv = M.pensar(d.ms, d.prof);
    postMessage({ tipo: 'jugada', mv, puntaje: M.puntaje(), prof: M.profundidad(), nodos: M.nodos_(), ms: performance.now() - t });
    return;
  }
  postMessage({ tipo: 'foto', ...foto(), id: d.id });
};

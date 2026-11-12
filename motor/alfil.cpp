// ALFIL: motor de ajedrez en C++.
// Mismo código para dos destinos:
//   nativo  -> g++ -O2 -o alfil motor/alfil.cpp            (protocolo UCI y perft)
//   web     -> clang --target=wasm32 -DALFIL_WASM ...       (ver compilar.sh)
// Tablero de 64 casillas, búsqueda alfa-beta con profundización iterativa,
// tabla de transposición (Zobrist), quiescencia, killers e historial.

#ifdef ALFIL_WASM
#define EXPORTA(n) extern "C" __attribute__((export_name(#n)))
extern "C" __attribute__((import_module("env"), import_name("ahora"))) double ahora();
extern "C" void *memset(void *d, int c, unsigned long n) { unsigned char *p = (unsigned char *)d; while (n--) *p++ = (unsigned char)c; return d; }
extern "C" void *memcpy(void *d, const void *s, unsigned long n) { unsigned char *p = (unsigned char *)d; const unsigned char *q = (const unsigned char *)s; while (n--) *p++ = *q++; return d; }
#else
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#define EXPORTA(n) extern "C"
static double ahora() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
#endif

typedef unsigned long long u64;
enum { VACIO = 0, P = 1, N = 2, B = 3, R = 4, Q = 5, K = 6 };
static const int MATE = 30000, INF = 32000;

// ---------------- estado ----------------
static signed char bd[64];
static int lado = 1, enroque = 15, ep = -1, medio = 0;
static u64 clave;
struct Deshacer { int mv; signed char cap; char flag; int enroque, ep, medio; u64 clave; };
static Deshacer pila[2048];
static int npila = 0;
static u64 zp[13][64], zlado, zenr[16], zep[8];
static int mascara[64];

static inline int abs_(int x) { return x < 0 ? -x : x; }
static inline int fila(int s) { return s >> 3; }
static inline int col(int s) { return s & 7; }
static inline int dentro(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }
static inline int De(int m) { return m & 63; }
static inline int A(int m) { return (m >> 6) & 63; }
static inline int Pr(int m) { return (m >> 12) & 7; }
static inline int Mv(int de, int a, int pr = 0) { return de | (a << 6) | (pr << 12); }

static u64 semilla = 0x9E3779B97F4A7C15ULL;
static u64 azar() { semilla ^= semilla << 13; semilla ^= semilla >> 7; semilla ^= semilla << 17; return semilla; }

static void iniciar_zobrist() {
  for (int p = 0; p < 13; p++) for (int s = 0; s < 64; s++) zp[p][s] = azar();
  zlado = azar();
  for (int i = 0; i < 16; i++) zenr[i] = azar();
  for (int i = 0; i < 8; i++) zep[i] = azar();
  for (int s = 0; s < 64; s++) mascara[s] = 15;
  mascara[4] &= ~3; mascara[7] &= ~1; mascara[0] &= ~2;
  mascara[60] &= ~12; mascara[63] &= ~4; mascara[56] &= ~8;
}

static void calcular_clave() {
  clave = 0;
  for (int s = 0; s < 64; s++) if (bd[s]) clave ^= zp[bd[s] + 6][s];
  if (lado < 0) clave ^= zlado;
  clave ^= zenr[enroque];
  if (ep >= 0) clave ^= zep[col(ep)];
}

static int fen(const char *f) {
  for (int s = 0; s < 64; s++) bd[s] = 0;
  int r = 7, c = 0;
  while (*f && *f != ' ') {
    char ch = *f++;
    if (ch == '/') { r--; c = 0; continue; }
    if (ch >= '1' && ch <= '8') { c += ch - '0'; continue; }
    int sg = (ch >= 'a') ? -1 : 1; char l = ch >= 'a' ? ch - 32 : ch;
    int t = l == 'P' ? P : l == 'N' ? N : l == 'B' ? B : l == 'R' ? R : l == 'Q' ? Q : l == 'K' ? K : 0;
    if (!t || !dentro(c, r)) return 0;
    bd[r * 8 + c++] = (signed char)(t * sg);
  }
  while (*f == ' ') f++;
  lado = (*f == 'b') ? -1 : 1; if (*f) f++;
  while (*f == ' ') f++;
  enroque = 0;
  while (*f && *f != ' ') { if (*f == 'K') enroque |= 1; if (*f == 'Q') enroque |= 2; if (*f == 'k') enroque |= 4; if (*f == 'q') enroque |= 8; f++; }
  while (*f == ' ') f++;
  ep = -1;
  if (*f >= 'a' && *f <= 'h') { ep = (f[1] - '1') * 8 + (f[0] - 'a'); f += 2; } else if (*f) f++;
  while (*f == ' ') f++;
  medio = 0; while (*f >= '0' && *f <= '9') medio = medio * 10 + (*f++ - '0');
  npila = 0; calcular_clave();
  return 1;
}

// ---------------- ataques ----------------
static const int NV[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
static const int RV[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

static int atacada(int s, int por) {
  int f = col(s), r = fila(s);
  int rp = r - por; // fila desde donde atacaría un peón de "por"
  if (rp >= 0 && rp < 8) {
    if (f > 0 && bd[rp * 8 + f - 1] == P * por) return 1;
    if (f < 7 && bd[rp * 8 + f + 1] == P * por) return 1;
  }
  for (int i = 0; i < 8; i++) {
    int nf = f + NV[i][0], nr = r + NV[i][1];
    if (dentro(nf, nr) && bd[nr * 8 + nf] == N * por) return 1;
    nf = f + RV[i][0]; nr = r + RV[i][1];
    if (dentro(nf, nr) && bd[nr * 8 + nf] == K * por) return 1;
  }
  for (int i = 0; i < 8; i++) {
    int df = RV[i][0], dr = RV[i][1], nf = f + df, nr = r + dr;
    while (dentro(nf, nr)) {
      int p = bd[nr * 8 + nf];
      if (p) {
        if (p * por > 0) { int t = p * por; if (t == Q || (i < 4 && t == R) || (i >= 4 && t == B)) return 1; }
        break;
      }
      nf += df; nr += dr;
    }
  }
  return 0;
}

static int rey(int c) { for (int s = 0; s < 64; s++) if (bd[s] == K * c) return s; return -1; }
static int en_jaque(int c) { int k = rey(c); return k >= 0 && atacada(k, -c); }

// ---------------- generación ----------------
static int generar(int *m, int solo_capturas) {
  int n = 0, c = lado;
  for (int s = 0; s < 64; s++) {
    int p = bd[s] * c; if (p <= 0) continue;
    int f = col(s), r = fila(s);
    if (p == P) {
      int r1 = r + c, ult = (c > 0) ? 7 : 0, ini = (c > 0) ? 1 : 6;
      if (r1 < 0 || r1 > 7) continue;
      if (!bd[r1 * 8 + f]) {
        if (r1 == ult) { for (int pr = Q; pr >= N; pr--) m[n++] = Mv(s, r1 * 8 + f, pr); }
        else if (!solo_capturas) {
          m[n++] = Mv(s, r1 * 8 + f);
          if (r == ini && !bd[(r + 2 * c) * 8 + f]) m[n++] = Mv(s, (r + 2 * c) * 8 + f);
        }
      }
      for (int df = -1; df <= 1; df += 2) {
        int nf = f + df; if (nf < 0 || nf > 7) continue;
        int t = r1 * 8 + nf;
        if (bd[t] * c < 0 || t == ep) {
          if (r1 == ult) { for (int pr = Q; pr >= N; pr--) m[n++] = Mv(s, t, pr); }
          else m[n++] = Mv(s, t);
        }
      }
    } else if (p == N || p == K) {
      const int (*V)[2] = p == N ? NV : RV;
      for (int i = 0; i < 8; i++) {
        int nf = f + V[i][0], nr = r + V[i][1];
        if (!dentro(nf, nr)) continue;
        int t = nr * 8 + nf;
        if (bd[t] * c > 0) continue;
        if (solo_capturas && !bd[t]) continue;
        m[n++] = Mv(s, t);
      }
      if (p == K && !solo_capturas) {
        int b = (c > 0) ? 0 : 56, ck = (c > 0) ? 1 : 4, cq = (c > 0) ? 2 : 8;
        if (s == b + 4 && (enroque & ck) && !bd[b + 5] && !bd[b + 6] && bd[b + 7] == R * c && !atacada(b + 4, -c) && !atacada(b + 5, -c) && !atacada(b + 6, -c)) m[n++] = Mv(s, b + 6);
        if (s == b + 4 && (enroque & cq) && !bd[b + 3] && !bd[b + 2] && !bd[b + 1] && bd[b] == R * c && !atacada(b + 4, -c) && !atacada(b + 3, -c) && !atacada(b + 2, -c)) m[n++] = Mv(s, b + 2);
      }
    } else {
      int i0 = (p == B) ? 4 : 0, i1 = (p == R) ? 4 : 8;
      for (int i = i0; i < i1; i++) {
        int nf = f + RV[i][0], nr = r + RV[i][1];
        while (dentro(nf, nr)) {
          int t = nr * 8 + nf;
          if (bd[t] * c > 0) break;
          if (bd[t]) { m[n++] = Mv(s, t); break; }
          if (!solo_capturas) m[n++] = Mv(s, t);
          nf += RV[i][0]; nr += RV[i][1];
        }
      }
    }
  }
  return n;
}

// ---------------- hacer y deshacer ----------------
static void hacer(int m) {
  int de = De(m), a = A(m), pr = Pr(m);
  signed char pc = bd[de], cp = bd[a];
  Deshacer &u = pila[npila++];
  u.mv = m; u.cap = cp; u.flag = 0; u.enroque = enroque; u.ep = ep; u.medio = medio; u.clave = clave;
  int t = abs_(pc);
  clave ^= zp[pc + 6][de];
  if (cp) clave ^= zp[cp + 6][a];
  bd[a] = pc; bd[de] = 0;
  medio = (t == P || cp) ? 0 : medio + 1;
  if (t == P && a == ep && !cp) { // captura al paso
    int cs = a - 8 * lado; u.cap = bd[cs]; u.flag = 1; clave ^= zp[bd[cs] + 6][cs]; bd[cs] = 0;
  }
  if (pr) bd[a] = (signed char)(pr * lado);
  clave ^= zp[bd[a] + 6][a];
  if (t == K && abs_(a - de) == 2) { // enroque: mover la torre
    int rd = a > de ? de + 3 : de - 4, ra = a > de ? de + 1 : de - 1;
    clave ^= zp[bd[rd] + 6][rd]; bd[ra] = bd[rd]; bd[rd] = 0; clave ^= zp[bd[ra] + 6][ra]; u.flag = 2;
  }
  if (ep >= 0) clave ^= zep[col(ep)];
  ep = (t == P && abs_(a - de) == 16) ? (a + de) / 2 : -1;
  if (ep >= 0) clave ^= zep[col(ep)];
  clave ^= zenr[enroque]; enroque &= mascara[de] & mascara[a]; clave ^= zenr[enroque];
  lado = -lado; clave ^= zlado;
}

static void deshacer_() {
  Deshacer &u = pila[--npila];
  lado = -lado;
  int de = De(u.mv), a = A(u.mv);
  signed char pc = Pr(u.mv) ? (signed char)(P * lado) : bd[a];
  bd[de] = pc; bd[a] = 0;
  if (u.flag == 1) bd[a - 8 * lado] = u.cap;
  else bd[a] = u.cap;
  if (u.flag == 2) { int rd = a > de ? de + 3 : de - 4, ra = a > de ? de + 1 : de - 1; bd[rd] = bd[ra]; bd[ra] = 0; }
  enroque = u.enroque; ep = u.ep; medio = u.medio; clave = u.clave;
}

static int legal(int m) { hacer(m); int ok = !en_jaque(-lado); deshacer_(); return ok; }

static u64 perft(int d) {
  if (!d) return 1;
  int m[256], n = generar(m, 0); u64 t = 0;
  for (int i = 0; i < n; i++) { hacer(m[i]); if (!en_jaque(-lado)) t += perft(d - 1); deshacer_(); }
  return t;
}

// ---------------- evaluación ----------------
static const int VAL[7] = {0, 100, 320, 330, 500, 900, 0};
// tablas vistas desde las blancas, fila 8 arriba (índice 0 = a8)
static const int TP[64] = {0,0,0,0,0,0,0,0, 50,50,50,50,50,50,50,50, 10,10,20,30,30,20,10,10, 5,5,10,25,25,10,5,5, 0,0,0,20,20,0,0,0, 5,-5,-10,0,0,-10,-5,5, 5,10,10,-20,-20,10,10,5, 0,0,0,0,0,0,0,0};
static const int TN[64] = {-50,-40,-30,-30,-30,-30,-40,-50, -40,-20,0,0,0,0,-20,-40, -30,0,10,15,15,10,0,-30, -30,5,15,20,20,15,5,-30, -30,0,15,20,20,15,0,-30, -30,5,10,15,15,10,5,-30, -40,-20,0,5,5,0,-20,-40, -50,-40,-30,-30,-30,-30,-40,-50};
static const int TB[64] = {-20,-10,-10,-10,-10,-10,-10,-20, -10,0,0,0,0,0,0,-10, -10,0,5,10,10,5,0,-10, -10,5,5,10,10,5,5,-10, -10,0,10,10,10,10,0,-10, -10,10,10,10,10,10,10,-10, -10,5,0,0,0,0,5,-10, -20,-10,-10,-10,-10,-10,-10,-20};
static const int TR[64] = {0,0,0,0,0,0,0,0, 5,10,10,10,10,10,10,5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, -5,0,0,0,0,0,0,-5, 0,0,0,5,5,0,0,0};
static const int TQ[64] = {-20,-10,-10,-5,-5,-10,-10,-20, -10,0,0,0,0,0,0,-10, -10,0,5,5,5,5,0,-10, -5,0,5,5,5,5,0,-5, 0,0,5,5,5,5,0,-5, -10,5,5,5,5,5,0,-10, -10,0,5,0,0,0,0,-10, -20,-10,-10,-5,-5,-10,-10,-20};
static const int TKM[64] = {-30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -30,-40,-40,-50,-50,-40,-40,-30, -20,-30,-30,-40,-40,-30,-30,-20, -10,-20,-20,-20,-20,-20,-20,-10, 20,20,0,0,0,0,20,20, 20,30,10,0,0,10,30,20};
static const int TKF[64] = {-50,-40,-30,-20,-20,-30,-40,-50, -30,-20,-10,0,0,-10,-20,-30, -30,-10,20,30,30,20,-10,-30, -30,-10,30,40,40,30,-10,-30, -30,-10,30,40,40,30,-10,-30, -30,-10,20,30,30,20,-10,-30, -30,-30,0,0,0,0,-30,-30, -50,-30,-30,-30,-30,-30,-30,-50};
static const int *TABLA[7] = {0, TP, TN, TB, TR, TQ, TKM};

static int evaluar() { // desde el lado que mueve
  int pts = 0, piezas = 0, alfiles[2] = {0, 0};
  for (int s = 0; s < 64; s++) if (bd[s] && abs_(bd[s]) != P && abs_(bd[s]) != K) piezas += VAL[abs_(bd[s])];
  int final_ = piezas <= 1300;
  for (int s = 0; s < 64; s++) {
    int p = bd[s]; if (!p) continue;
    int c = p > 0 ? 1 : -1, t = p * c;
    int idx = c > 0 ? (7 - fila(s)) * 8 + col(s) : fila(s) * 8 + col(s);
    const int *tb = (t == K && final_) ? TKF : TABLA[t];
    pts += c * (VAL[t] + tb[idx]);
    if (t == B) alfiles[c > 0 ? 0 : 1]++;
    if (t == P) { // peones doblados y pasados (simple)
      int f = col(s), dob = 0, pasado = 1;
      for (int r = 0; r < 8; r++) if (r != fila(s) && bd[r * 8 + f] == p) dob = 1;
      for (int r = fila(s) + c; r >= 0 && r < 8; r += c)
        for (int df = -1; df <= 1; df++) if (f + df >= 0 && f + df < 8 && bd[r * 8 + f + df] == -p) pasado = 0;
      if (dob) pts -= c * 12;
      if (pasado) pts += c * (c > 0 ? fila(s) : 7 - fila(s)) * (final_ ? 14 : 7);
    }
  }
  if (alfiles[0] >= 2) pts += 30;
  if (alfiles[1] >= 2) pts -= 30;
  return pts * lado + 10;
}

// ---------------- búsqueda ----------------
struct Entrada { u64 k; int mv; short sc; signed char d; char tipo; };
static const int TT_N = 1 << 18;
static Entrada tt[TT_N];
static int killers[128][2];
static int historial[64][64];
static long nodos;
static int parar;
static double limite;
static int mejor_raiz, puntaje_raiz, prof_alcanzada;

static int repetida() {
  for (int i = npila - 2; i >= 0 && i >= npila - medio; i -= 2) if (pila[i].clave == clave) return 1;
  return 0;
}

static void ordenar(int *m, int n, int ttm, int ply) {
  int sc[256];
  for (int i = 0; i < n; i++) {
    int x = m[i], v = 0, c = bd[A(x)];
    if (x == ttm) v = 1 << 30;
    else if (c) v = (1 << 20) + VAL[abs_(c)] * 10 - VAL[abs_(bd[De(x)])] / 10;
    else if (Pr(x)) v = (1 << 20) + VAL[Pr(x)];
    else if (x == killers[ply][0]) v = 1 << 19;
    else if (x == killers[ply][1]) v = (1 << 19) - 1;
    else v = historial[De(x)][A(x)];
    sc[i] = v;
  }
  for (int i = 1; i < n; i++) { int x = m[i], s = sc[i], j = i - 1; while (j >= 0 && sc[j] < s) { m[j + 1] = m[j]; sc[j + 1] = sc[j]; j--; } m[j + 1] = x; sc[j + 1] = s; }
}

static void revisar_tiempo() { if ((nodos & 2047) == 0 && ahora() > limite) parar = 1; }

static int quiescencia(int a, int b, int ply) {
  nodos++; revisar_tiempo(); if (parar) return 0;
  int est = evaluar();
  if (est >= b) return b;
  if (est > a) a = est;
  if (ply > 60) return est;
  int m[256], n = generar(m, 1);
  ordenar(m, n, 0, ply < 127 ? ply : 127);
  for (int i = 0; i < n; i++) {
    hacer(m[i]);
    if (en_jaque(-lado)) { deshacer_(); continue; }
    int v = -quiescencia(-b, -a, ply + 1);
    deshacer_();
    if (parar) return 0;
    if (v >= b) return b;
    if (v > a) a = v;
  }
  return a;
}

static int buscar(int d, int a, int b, int ply) {
  if (ply && (medio >= 100 || repetida())) return 0;
  int jaque = en_jaque(lado);
  if (jaque) d++;
  if (d <= 0) return quiescencia(a, b, ply);
  nodos++; revisar_tiempo(); if (parar) return 0;
  Entrada &e = tt[clave & (TT_N - 1)];
  int ttm = 0;
  if (e.k == clave) {
    ttm = e.mv;
    if (ply && e.d >= d) {
      int v = e.sc; if (v > MATE - 200) v -= ply; if (v < -MATE + 200) v += ply;
      if (e.tipo == 0) return v;
      if (e.tipo == 1 && v >= b) return v;
      if (e.tipo == 2 && v <= a) return v;
    }
  }
  // movimiento nulo
  if (ply && !jaque && d >= 3 && evaluar() >= b) {
    int tiene = 0; for (int s = 0; s < 64; s++) if (bd[s] * lado > 1 && bd[s] * lado != K) { tiene = 1; break; }
    if (tiene) {
      int eps = ep; if (ep >= 0) clave ^= zep[col(ep)]; ep = -1; lado = -lado; clave ^= zlado;
      int v = -buscar(d - 3, -b, -b + 1, ply + 1);
      lado = -lado; clave ^= zlado; ep = eps; if (ep >= 0) clave ^= zep[col(ep)];
      if (parar) return 0;
      if (v >= b) return b;
    }
  }
  int m[256], n = generar(m, 0);
  ordenar(m, n, ttm, ply < 127 ? ply : 127);
  int legales = 0, mejor = -INF, mv_mejor = 0, a0 = a;
  for (int i = 0; i < n; i++) {
    hacer(m[i]);
    if (en_jaque(-lado)) { deshacer_(); continue; }
    legales++;
    int v;
    int tranquila = !pila[npila - 1].cap && !Pr(m[i]);
    if (legales > 4 && d >= 3 && tranquila && !jaque) { // reducción de movimientos tardíos
      v = -buscar(d - 2, -a - 1, -a, ply + 1);
      if (v > a) v = -buscar(d - 1, -b, -a, ply + 1);
    } else if (legales > 1) {
      v = -buscar(d - 1, -a - 1, -a, ply + 1);
      if (v > a && v < b) v = -buscar(d - 1, -b, -a, ply + 1);
    } else v = -buscar(d - 1, -b, -a, ply + 1);
    deshacer_();
    if (parar) return 0;
    if (v > mejor) { mejor = v; mv_mejor = m[i]; if (!ply) { mejor_raiz = m[i]; puntaje_raiz = v; } }
    if (v > a) a = v;
    if (a >= b) {
      if (tranquila && ply < 128) { if (killers[ply][0] != m[i]) { killers[ply][1] = killers[ply][0]; killers[ply][0] = m[i]; } historial[De(m[i])][A(m[i])] += d * d; }
      break;
    }
  }
  if (!legales) return jaque ? -MATE + ply : 0;
  int g = mejor; if (g > MATE - 200) g += ply; if (g < -MATE + 200) g -= ply;
  e.k = clave; e.mv = mv_mejor; e.sc = (short)g; e.d = (signed char)d; e.tipo = mejor >= b ? 1 : mejor <= a0 ? 2 : 0;
  return mejor;
}

static int pensar_(double ms, int prof_max) {
  nodos = 0; parar = 0; limite = ahora() + ms; mejor_raiz = 0; prof_alcanzada = 0;
  for (int i = 0; i < 128; i++) killers[i][0] = killers[i][1] = 0;
  for (int i = 0; i < 64; i++) for (int j = 0; j < 64; j++) historial[i][j] /= 8;
  int mejor = 0, pts = 0;
  for (int d = 1; d <= prof_max; d++) {
    buscar(d, -INF, INF, 0);
    if (parar && d > 1) break;
    mejor = mejor_raiz; pts = puntaje_raiz; prof_alcanzada = d;
    if (pts > MATE - 200 || pts < -MATE + 200) break;
    if (ahora() > limite - ms * .45) break; // no empezar otra que no alcanza
  }
  if (!mejor) { int m[256], n = generar(m, 0); for (int i = 0; i < n; i++) if (legal(m[i])) { mejor = m[i]; break; } }
  puntaje_raiz = pts;
  return mejor;
}

static int estado_() {
  int m[256], n = generar(m, 0), hay = 0;
  for (int i = 0; i < n && !hay; i++) hay = legal(m[i]);
  int j = en_jaque(lado);
  if (!hay) return j ? 2 : 3;
  if (medio >= 100) return 4;
  int rep = 0; for (int i = npila - 2; i >= 0 && i >= npila - medio; i -= 2) if (pila[i].clave == clave) rep++;
  if (rep >= 2) return 5;
  int menores = 0, otras = 0;
  for (int s = 0; s < 64; s++) { int t = abs_(bd[s]); if (t == N || t == B) menores++; else if (t && t != K) otras++; }
  if (!otras && menores <= 1) return 6;
  return j ? 1 : 0;
}

// ---------------- interfaz para la web ----------------
static int buf[256];
static const char *INICIO = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
static int listo = 0;
static void asegurar() { if (!listo) { iniciar_zobrist(); fen(INICIO); listo = 1; } }

EXPORTA(reiniciar) void reiniciar() { asegurar(); fen(INICIO); }
EXPORTA(pieza) int pieza(int s) { asegurar(); return bd[s]; }
EXPORTA(turno) int turno() { asegurar(); return lado; }
EXPORTA(buffer) int *buffer() { return buf; }
EXPORTA(legales) int legales() {
  asegurar(); int m[256], n = generar(m, 0), k = 0;
  for (int i = 0; i < n; i++) if (legal(m[i])) buf[k++] = m[i];
  return k;
}
EXPORTA(mover) int mover(int m) {
  int n = legales();
  for (int i = 0; i < n; i++) if (buf[i] == m) { hacer(m); return 1; }
  return 0;
}
EXPORTA(deshacer) int deshacer() { if (!npila) return 0; deshacer_(); return 1; }
EXPORTA(pensar) int pensar(double ms, int prof) { asegurar(); return pensar_(ms, prof); }
EXPORTA(puntaje) int puntaje() { return puntaje_raiz; }
EXPORTA(profundidad) int profundidad() { return prof_alcanzada; }
EXPORTA(nodos_) double nodos_() { return (double)nodos; }
EXPORTA(estado) int estado() { asegurar(); return estado_(); }
EXPORTA(evaluacion) int evaluacion() { asegurar(); return evaluar() * lado; }

// ---------------- UCI (solo nativo) ----------------
#ifndef ALFIL_WASM
static std::string a_uci(int m) {
  std::string s; s += char('a' + col(De(m))); s += char('1' + fila(De(m))); s += char('a' + col(A(m))); s += char('1' + fila(A(m)));
  if (Pr(m)) s += " nbrq"[Pr(m) - 1];
  return s;
}
static int de_uci(const std::string &t) {
  int n = legales();
  for (int i = 0; i < n; i++) if (a_uci(buf[i]) == t) return buf[i];
  return 0;
}
int main(int argc, char **argv) {
  asegurar();
  if (argc > 1 && std::string(argv[1]) == "perft") {
    if (argc > 3) fen(argv[3]);
    int d = argc > 2 ? atoi(argv[2]) : 4; double t = ahora();
    u64 r = perft(d); printf("perft %d = %llu (%.0f ms)\n", d, r, ahora() - t); return 0;
  }
  std::string linea;
  while (std::getline(std::cin, linea)) {
    std::istringstream is(linea); std::string cmd; is >> cmd;
    if (cmd == "uci") printf("id name Alfil 1.0\nid author NeckHurtttt\nuciok\n");
    else if (cmd == "isready") printf("readyok\n");
    else if (cmd == "ucinewgame") { for (int i = 0; i < TT_N; i++) tt[i] = Entrada(); }
    else if (cmd == "position") {
      std::string t, f; is >> t;
      if (t == "startpos") { fen(INICIO); is >> t; }
      else if (t == "fen") { while (is >> t && t != "moves") f += t + " "; fen(f.c_str()); }
      if (t == "moves") while (is >> t) { int m = de_uci(t); if (m) hacer(m); }
    } else if (cmd == "go") {
      std::string t; double ms = 3000; int d = 64, wt = -1, bt = -1;
      while (is >> t) {
        if (t == "movetime") is >> ms; else if (t == "depth") is >> d;
        else if (t == "wtime") is >> wt; else if (t == "btime") is >> bt;
      }
      int resto = lado > 0 ? wt : bt; if (resto > 0) ms = resto / 30.0;
      int m = pensar_(ms, d);
      printf("info depth %d score cp %d nodes %ld\nbestmove %s\n", prof_alcanzada, puntaje_raiz, nodos, a_uci(m).c_str());
    } else if (cmd == "d") {
      for (int r = 7; r >= 0; r--) { for (int f = 0; f < 8; f++) { int p = bd[r * 8 + f]; putchar(p ? " PNBRQK"[abs_(p)] + (p < 0 ? 32 : 0) : '.'); } putchar('\n'); }
    } else if (cmd == "quit") break;
    fflush(stdout);
  }
}
#endif

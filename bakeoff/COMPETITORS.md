# Análisis competitivo: zstd, lizard, misa77 (vs lz6)

Corpus: AIT A-H (13,136,308 bytes), single-threaded, medición mem-to-mem.
Externos vía lzbench 1.2 (`-t2,2`); lz6 seq vía `bakeoff/bench_seq.c` (misma
metodología mem-to-mem, MB/s corregidos a bytes de salida). Fecha: 2026-08-28,
rev lz6: post f638c16. gzip -6 medido aparte como ancla de Weissman.

## Tabla agregada (ordenada por ratio)

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| **lz6 seq L15** | **5,361,732** | **40.82%** | ~15 | ~222 |
| **lz6 seq L2 (default)** | **5,596,570** | **42.60%** | ~120 | ~262 |
| zstd 1.5.7 -9 | 7,578,176 | 57.69% | 61 | 985 |
| zstd 1.5.7 -3 | 7,695,386 | 58.58% | 183 | 1,033 |
| zstd 1.5.7 -1 | 7,869,981 | 59.91% | 413 | 1,322 |
| gzip -6 (ancla) | 7,820,934 | 59.53% | 15 | ~110-400 |
| lizard 2.1 -45 | 8,253,375 | 62.83% | 15 | 1,490 |
| **lz6 --hc -15 (frame)** | 8,588,303 | 65.38% | 5 | 1,016 |
| lizard 2.1 -30 | 8,756,123 | 66.66% | 428 | 1,865 |
| misa77 0.6.0 -4 | 9,070,575 | 69.05% | 6 | 3,593 |
| misa77 0.6.0 -2 | 9,287,323 | 70.70% | 36 | 6,519 |
| misa77 0.6.0 -1 | 9,628,088 | 73.29% | 49 | 7,761 |
| lizard 2.1 -20 | 9,761,531 | 74.31% | 429 | 2,398 |
| lizard 2.1 -10 | 9,926,082 | 75.56% | 597 | 4,640 |
| lz4 1.10.0 | 9,938,605 | 75.66% | 739 | 4,985 |
| misa77 0.6.0 --1 | 10,041,718 | 76.44% | 309 | 7,384 |

## Lectura por competidor

### misa77 (la amenaza de decode)

Tesis del proyecto: "write-once, read-many" — decode memcpy-class a costa de
encode lento. En el agregado de AIT decodifica a 7.7 GB/s (L1) con 73.29%:
**30x más rápido que nuestro seq, 31 pts peor ratio**.

Dónde nos pisa (texto):
- C: misa -1 = 42.31% @ 5,436 MB/s vs lz6 seq L2 = 37.71% @ 180 (-4.6 pts ratio, 30x decode)
- B: misa -1 = 31.48% @ 6,366 vs lz6 29.85% @ 232 (-1.6 pts, 27x)
- H: misa -1 = 57.46% @ 5,155 vs lz6 57.40% @ 179 (**empate en ratio, 29x decode**)

Dónde falla (nuestras fortalezas estructurales):
- A (binario de alta entropía): misa -1 = 99.47% (no comprime); lz6 seq = 53.14%
- E: misa = 100.00%; lz6 = 79.76% (plane transform)
- F: misa = 100.00%; lz6 = 79.29%
- D (PRNG): misa = 100.00%; lz6 = 0.00% (regeneración desde seed, 7 bytes)

Causa raíz: el formato light de misa77 no tiene codificación de entropía de
literales (LZ puro con encoding de alto ancho de banda). Todo su decode es
copy-loop sin mesa de FSE/rANS. Nuestro ratio viene de exactamente lo que
frena nuestro decode.

Veredicto Weissman (AIT, ratio×speed vs gzip 59.53%): misa77 -1 tiene
ratio_factor = 59.53/73.29 = 0.81 (<1: peor que gzip en ratio) pero
speed_factor enorme. En archivos de texto puro la combinación puede
superarnos; en el agregado AIT nuestro ratio_factor 1.40 con decode 2-3x
gzip sostiene el #1 del challenge. El leaderboard real decide: si misa77
entrara al challenge compitiendo por archivo, sería rival directo en B/C/H.

### zstd (el generalista)

Sigue siendo el #2 en ratio (57.69% a -9) y el mejor equilibrio general.
Decodifica 3.8x más rápido que nuestro seq con 15 pts peor ratio. Su rango
-1..-3 (58.6-59.9% @ 1.0-1.3 GB/s dec) es el punto de comparación comercial
natural. En A no tiene rival entre los externos a baja velocidad (52.41% a -1,
mejor que nuestro 53.14%).

### lizard (el ancestro)

Fork de lz5 con modos de entropía: -30 (FSE) = 66.66% @ 1,865 dec; -45
(máximo) = 62.83% @ 1,490. Nuestro seq lo supera por 20+ pts de ratio a
igual clase de encode. Confirmación empírica de que la línea lz5+lizard
estaba a mitad de camino de lo que el seq ya implementó. Su valor hoy es
histórico/arqueológico: mirar qué hizo lizard -30 (FSE literals) y por qué
se quedó corto frente a nuestro pipeline completo.

### lz4

Referencia de velocidad pura. Nuestro frame default (fast, ~76% @ 739/4,985)
es su clase; nuestro seq lo bate por 33 pts de ratio con 7x menos decode.

## Oportunidades (ordenadas por impacto esperado)

1. **Decode del seq es LA brecha**: 262 MB/s vs 1-8 GB/s de la competencia.
   Palancas ya identificadas (ver plan Weissman): streaming decode
   (no materializar arrays), wildcopy en copias, batched refills, y la idea
   misa77-style de maximizar ancho de copia por secuencia. Cada MB/s de
   decode sube el Weissman directo.
2. **Formato híbrido LZ6S2**: bloques seq para ratio + bloques light-style
   (LZ puro, decode memcpy-class) para bloques que el parse marque como
   "fáciles" — el decoder elegiría camino por bloque. El frame ya soporta
   codecs por bloque; solo falta un selector en el encoder.
3. **Literales raw más inteligentes**: A/H/E/F muestran que cuando los
   literales no comprimen, pagamos el costo mínimo pero misa77 ni intenta.
   Nuestras victorias ahí (plane, PRNG) son jáquer transformaciones — más de
   eso (delta para binario estructurado, RGB/float lanes) amplía la brecha.
4. **No perseguir el decode de misa77 en ratio**: su ventaja viene de NO
   tener entropía; copiar su diseño nos haría perder los 15 pts de ratio
   frente a zstd, que es nuestra única corona defensible.

## Metodología y caveat

- lzbench mide mem-to-mem sin I/O; bench_seq ídem (MB/s de decode corregidos
  por tamaño de salida). Los números entre harnesses son comparables en orden
  de magnitud, no al pie.
- El ruido de decode en esta máquina es ±5% (ver SEQ_SPEED_BASELINE.txt);
  usar los canarios D/E para comparaciones finas.
- misa77 0.6.0 (13 commits, v0.x): formato inestable, sin safe-decoder para
  su nivel 4. Riesgo de adopción alto para terceros, pero la ingeniería de
  decode es real y su presencia en lzbench/TurboBench le da visibilidad.


---

## Round 2 (post LZ6S2): Silesia.tar — el espejo que revela el sesgo AIT

Corpus: Silesia.tar (211,957,760 bytes, 12 archivos heterogéneos). Misma
metodología; gzip -6 como ancla (25 enc / ~174 dec MB/s).

| codec | csize | ratio | enc MB/s | dec MB/s |
|---|---:|---:|---:|---:|
| zstd -9 | 59,071,826 | 27.87% | 53 | 656 |
| **lz6 seq L15** | **62,437,010** | **29.46%** | **2.2** | **141** |
| lizard -45 | 66,676,865 | 31.46% | 18.8 | 1,078 |
| zstd -3 | 66,133,605 | 31.20% | 158 | 702 |
| **lz6 --hc -15 (frame)** | **65,237,073** | **30.78%** | **2.7** | **994** |
| gzip -6 (ancla) | 68,235,411 | 32.19% | 25 | ~174 |
| zstd -1 | 73,193,861 | 34.53% | 348 | 1,135 |
| misa77 -4 | 75,259,843 | 35.51% | 6.6 | 1,028 |
| **lz6 seq L2** | **90,730,094** | **42.81%** | **99** | **272** |
| misa77 -1 | 90,386,470 | 42.64% | 49.3 | 4,131 |
| lz4 | 100,881,076 | 47.59% | 512 | 3,244 |
| lizard -10 | 103,401,614 | 48.78% | 433 | 2,984 |

### Hallazgos que cambian el roadmap

1. **La corona de ratio es específica de AIT.** En AIT nuestro L2 (42.61%)
   aplasta a zstd -1 (59.91%); en Silesia zstd -1 (34.53%) nos pasa por
   arriba con 8 puntos y decodifica 4x más rápido. La brecha se concentra
   en los binarios grandes (mozilla 51MB, nci): literal coding y match
   strategy genéricas de zstd vs nuestra dependencia de transforms
   específicas (plane/PRNG) que no cubren estos datos.

2. **Saturación de hash table a escala.** Con 100MB+ de input y 8M buckets
   (L2), las cadenas se saturan y la búsqueda shallow (searchNum=2) agarra
   candidatos recientes de baja calidad. Medido: en el slice de 100MB,
   bloques de 16MB (cadenas cortas) comprimen 4.5 pts MEJOR que bloques de
   64/100MB con la misma ventana de 32MB. En mozilla-type data el efecto
   domina.

3. **El default B6 del CLI era el peor punto para seq en Silesia** (48.58%
   vs 42.81% de B7 — 12MB de diferencia por dead zones de frontera con la
   ventana de 32MB). El default seq ahora es B7 (bloque único hasta 256MB,
   ~3.5GB RAM); AIT sin cambios (bloque único igual).

4. **El HC frame (L15) es competitivo en Silesia** (30.78% @ 994 MB/s dec —
   7x nuestro seq L15 con 1.3 pts peor ratio): el decoder LZ del frame con
   wildcopy sigue siendo la máquina de decode rápido del proyecto.

### Roadmap revisado (por evidencia)

1. **Literal coding para binarios** — el gap de Silesia vive en mozilla/nci:
   FSE literals con contexts binarios (zstd-style offsets/extended contexts)
   o el modo order-1 generalizado con lazyness de tablas.
2. **Matcher a escala** — searchNum adaptativo a la saturación de cadenas o
   hash más ancho en niveles bajos: recupera ratio en archivos grandes sin
   tocar encode speed en los chicos.
3. **Weissman del challenge**: re-corrida AIT con la build final dio L2
   5,597,278 / L15 5,362,556 — sin cambios vs lo registrado.

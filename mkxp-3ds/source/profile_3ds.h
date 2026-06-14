/* ============================================================================
 * profile_3ds.h — Sistema de profiling para isolar onde o tempo e' gasto.
 * ----------------------------------------------------------------------------
 * Em vez de INFERIR tempos a partir de timestamps espalhados pelo log (ex:
 * "227 allocs/segundo"), este modulo MEDE diretamente cada fase com o relogio
 * do 3DS (svcGetSystemTick @ 268MHz) e imprime relatorios [PROFILE] organizados.
 *
 * Mede 4 dominios:
 *   1. MARSHAL  — por ficheiro: bytes, tempo total, tempo a ler vs criar objetos,
 *                 nº de objetos, nº de GCs disparados durante o parse.
 *   2. GC       — global: nº de execucoes do GC, tempo total no GC, objetos vivos.
 *   3. FRAME    — runtime: tempo update (Ruby) vs render (C++), objetos/frame.
 *   4. RENDER   — por frame: begin_frame, sprites_draw_all, end_frame.
 *
 * Uso: chamar PROF_ZONE_BEGIN(zona) / PROF_ZONE_END(zona) a' volta do codigo a
 * medir. Os tempos somam-se em acumuladores. PROF_REPORT() imprime tudo.
 *
 * Custo: cada par begin/end e' 2 leituras de tick (baratissimo, ~nanosegundos).
 * NAO faz I/O — so' soma inteiros. O I/O acontece apenas no PROF_REPORT periodico.
 * Por isso o profiling em si NAO falseia as medicoes (ao contrario de logar por
 * alocacao, que escrevia no SD).
 * ========================================================================== */
#ifndef PROFILE_3DS_H
#define PROFILE_3DS_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Relogio (igual ao usado em binding_3ds/display_3ds) ----------------- */
#ifdef __3DS__
#include <3ds.h>
static inline uint64_t prof_tick(void) { return svcGetSystemTick(); }
#else
/* Fora do 3DS (testes no servidor): usa um contador monotonico qualquer. */
#include <time.h>
static inline uint64_t prof_tick(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 268111856ull + (uint64_t)ts.tv_nsec * 268111856ull / 1000000000ull;
}
#endif
static inline double prof_ticks_to_ms(uint64_t t) { return (double)t / 268111.856; }

/* ---- Acumuladores por zona ----------------------------------------------- *
 * Cada zona tem: tempo total acumulado (ticks), nº de vezes que correu, e um
 * tick de inicio (para o begin/end). Sao globais para serem partilhados entre
 * ficheiros (.cpp) sem instanciar nada. */
typedef struct {
    uint64_t total_ticks;   /* soma de todos os intervalos medidos */
    uint64_t count;         /* quantas vezes a zona correu */
    uint64_t t_begin;       /* tick do ultimo BEGIN (interno) */
} ProfZone;

/* As zonas concretas. Definidas (uma vez) em profile_3ds.cpp; referidas como
 * extern aqui para todos os ficheiros poderem incrementa-las. */
typedef enum {
    PROF_MARSHAL_TOTAL = 0,   /* tempo total do marshalLoadInt */
    PROF_MARSHAL_READ,        /* tempo so' a ler bytes (read_value/readByte) */
    PROF_MARSHAL_ALLOC,       /* tempo so' em mrb_obj_alloc (criar objeto) */
    PROF_MARSHAL_IVSET,       /* tempo so' em mrb_obj_iv_set (setar ivars) */
    PROF_MARSHAL_CLASS,       /* tempo so' em class_from_path (resolver classe) */
    PROF_MARSHAL_SYMBOL,      /* tempo so' em read_symbol (mrb_intern_cstr) */
    PROF_MARSHAL_STRING,      /* tempo so' em read_str_val (mrb_str_new) */
    PROF_GC,                  /* tempo dentro do GC (se instrumentado) */
    PROF_FRAME_UPDATE,        /* tempo da logica do frame (Ruby update) */
    PROF_FRAME_RENDER,        /* tempo do render do frame (C++) */
    PROF_RENDER_BEGIN,        /* begin_frame */
    PROF_RENDER_DRAW,         /* sprites_draw_all */
    PROF_RENDER_END,          /* end_frame */
    PROF_ZONE_COUNT
} ProfZoneId;

extern ProfZone g_prof_zones[PROF_ZONE_COUNT];

/* Contadores auxiliares (nao-tempo): preenchidos pelo codigo instrumentado. */
extern uint64_t g_prof_objs_created;   /* objetos criados (marshal/runtime) */
extern uint64_t g_prof_gc_runs;        /* nº de vezes que o GC correu */
extern uint64_t g_prof_bytes_parsed;   /* bytes processados pelo marshal */
extern uint64_t g_prof_blits;          /* blits de sprites no render */
extern uint64_t g_prof_frees;          /* frees (size=0) = proxy da atividade do GC */

/* ---- API ----------------------------------------------------------------- */
static inline void prof_zone_begin(ProfZoneId z) {
    g_prof_zones[z].t_begin = prof_tick();
}
static inline void prof_zone_end(ProfZoneId z) {
    uint64_t now = prof_tick();
    g_prof_zones[z].total_ticks += (now - g_prof_zones[z].t_begin);
    g_prof_zones[z].count++;
}

/* Macros convenientes */
#define PROF_BEGIN(z) prof_zone_begin(z)
#define PROF_END(z)   prof_zone_end(z)

/* Reset de uma zona (ex: por ficheiro de marshal) */
static inline void prof_zone_reset(ProfZoneId z) {
    g_prof_zones[z].total_ticks = 0;
    g_prof_zones[z].count = 0;
}

/* Imprime um relatorio organizado no log (g_dbglog) + stdout.
 * Definido em profile_3ds.cpp porque acede a g_dbglog. */
void prof_report(const char *tag);

/* Relatorio especifico de UM ficheiro de marshal (chamado no fim de cada load).
 * bytes/objs/frees dao o detalhe que faltava: ms/byte, ms/objeto, e se o GC
 * esteve ativo durante o parse (frees = proxy do GC). */
void prof_report_marshal(const char *fname, uint64_t bytes,
                         uint64_t objs, uint64_t frees,
                         double total_ms);

/* ============================================================================
 * PROFILING DE SCRIPTS / PLUGINS — o equivalente ao BREAKDOWN dos rxdata.
 * ----------------------------------------------------------------------------
 * Para cada script (ou plugin) registamos onde foi o tempo: descomprimir,
 * aplicar patch, compilar (cache miss) ou carregar bytecode (cache hit), e
 * executar. No fim, prof_report_scripts() imprime:
 *   - hits vs miss (a cache de bytecode esta' a funcionar?)
 *   - tempo total por fase (compilar vs executar = otimizavel vs inevitavel)
 *   - os N scripts MAIS LENTOS (para saber quais cortar no 3DS)
 * ========================================================================== */

/* Regista um script processado. Chamar uma vez por script.
 *   name     : nome do script/plugin
 *   bytes    : tamanho do codigo (descomprimido)
 *   cache_hit: true se veio do bytecode .mrb; false se compilou
 *   decomp_ms: tempo a descomprimir (zlib)
 *   patch_ms : tempo a aplicar patches
 *   exec_ms  : tempo a executar (load_irep_buf no hit, ou vm_run no miss)
 *   compile_ms: tempo a compilar+gravar bytecode (0 no hit) */
void prof_script_record(const char *name, uint64_t bytes, int cache_hit,
                        double decomp_ms, double patch_ms,
                        double exec_ms, double compile_ms);

/* Imprime o relatorio agregado + os scripts mais lentos. Chamar no fim do loop
 * de scripts E no fim do loop de plugins (tag distingue-os). */
void prof_report_scripts(const char *tag);

/* Limpa os registos de scripts (para separar a fase de scripts da de plugins). */
void prof_scripts_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PROFILE_3DS_H */

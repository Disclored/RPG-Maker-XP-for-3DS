/* ============================================================================
 * debug_3ds.h  --  Super Debug System (global, extensivel)  [Tarefa 7]
 * ----------------------------------------------------------------------------
 * Sistema de logging persistente e por-categoria, partilhado por TODOS os
 * ficheiros do port (binding, display, input, sprite, bitmap, marshal, ...).
 *
 * Objetivo: diagnosticar varios problemas numa unica execucao em vez de 1 a 1.
 *
 * Uso:
 *   #include "debug_3ds.h"
 *   DBG(DBG_INPUT, "A trigger=%d", t);
 *   DBG(DBG_RENDER, "frame %d blits=%d", f, n);
 *   DBGV(DBG_DISPLAY)  // so testa se a categoria esta ativa (para blocos caros)
 *
 * Cada categoria pode ser ligada/desligada em runtime (DBG_MASK) sem recompilar
 * tudo -- so muda a mascara. O output vai para o mesmo ficheiro de log global
 * (sdmc:/mkxp/debug_binding.log) com prefixo de categoria e contador de frame.
 *
 * Performance: o flush e' periodico (partilhado com g_dbglog_flushc). Categorias
 * desligadas custam um teste de bit (~0). Em release podes definir
 * DBG_COMPILE_OUT para remover todo o logging em tempo de compilacao.
 * ============================================================================ */
#ifndef DEBUG_3DS_H
#define DEBUG_3DS_H

#include <stdio.h>
#include <stdint.h>

/* Log global e contador de flush. Definidos no binding_3ds.cpp com linkage C++
 * (fora de extern "C"), por isso declaramo-los aqui ANTES do extern "C" para
 * o linkage bater certo (senao o g++ da' "conflicting declaration with C linkage"). */
extern FILE* g_dbglog;
extern int   g_dbglog_flushc;

#ifdef __cplusplus
extern "C" {
#endif

/* Contador de frame global, atualizado pelo display a cada frame, para que
 * QUALQUER categoria possa carimbar a que frame pertence o evento. */
extern volatile int g_dbg_frame;

/* ── Categorias (bitmask) ─────────────────────────────────────────────────── */
enum {
    DBG_INPUT     = 1u << 0,   /* teclas, trigger/press/release, mapeamento     */
    DBG_DISPLAY   = 1u << 1,   /* begin/end frame, targets, present             */
    DBG_RENDER    = 1u << 2,   /* blits, draw calls, ordem                      */
    DBG_BITMAP    = 1u << 3,   /* load/criacao de bitmaps, dimensoes            */
    DBG_TEXTURE   = 1u << 4,   /* C3D_TexInit/Delete, POT, morton               */
    DBG_VRAM      = 1u << 5,   /* alocacao/uso de VRAM e linearAlloc            */
    DBG_SCALE     = 1u << 6,   /* escala, offset, viewport, clipping            */
    DBG_POS       = 1u << 7,   /* posicionamento de sprites (x,y,ox,oy,z)       */
    DBG_SPRITE    = 1u << 8,   /* ciclo de vida de sprites                      */
    DBG_CACHE     = 1u << 9,   /* cache de bytecode, hits/miss/dump             */
    DBG_COMPILE   = 1u << 10,  /* compilacao de scripts, plugins               */
    DBG_TIMING    = 1u << 11,  /* tempos de carregamento                       */
    DBG_RESOURCE  = 1u << 12,  /* recursos em falta (ficheiros, bitmaps)       */
    DBG_STATE     = 1u << 13,  /* estados invalidos, asserts suaves            */
    DBG_ERROR     = 1u << 14,  /* erros silenciosos, fallbacks                 */
    DBG_MARSHAL   = 1u << 15,  /* marshal load/reconstrucao                     */
    DBG_AUDIO     = 1u << 16,  /* (futuro) audio                                */
    DBG_ALL       = 0xFFFFFFFFu
};

/* Mascara ativa. Por defeito: tudo o que ajuda a diagnosticar sem inundar.
 * Podes mudar em runtime: dbg_set_mask(DBG_INPUT | DBG_RENDER); */
extern uint32_t g_dbg_mask;

/* Remover TODO o logging em compilacao (build de release): define isto. */
#ifndef DBG_COMPILE_OUT

/* Escreve uma linha de log se a categoria estiver ativa. Prefixo: [CAT|f<frame>] */
#define DBG(cat, fmt, ...) do { \
    if (g_dbglog && (g_dbg_mask & (cat))) { \
        fprintf(g_dbglog, "[%s|f%d] " fmt "\n", dbg_cat_name(cat), g_dbg_frame, ##__VA_ARGS__); \
        if (++g_dbglog_flushc >= 512) { g_dbglog_flushc = 0; fflush(g_dbglog); } \
    } \
} while (0)

/* Testa se uma categoria esta ativa (para evitar calculos caros antes de logar). */
#define DBGV(cat) (g_dbglog && (g_dbg_mask & (cat)))

#else
#define DBG(cat, fmt, ...) do {} while (0)
#define DBGV(cat) (0)
#endif

/* Nome curto da categoria (para o prefixo). */
const char* dbg_cat_name(uint32_t cat);

/* Ajustar a mascara em runtime. */
void dbg_set_mask(uint32_t mask);
uint32_t dbg_get_mask(void);

/* Marcar o frame atual (chamado pelo display a cada begin_frame). */
void dbg_set_frame(int frame);

/* Medicao de tempo simples (ms desde o boot, via svcGetSystemTick nao -- usamos
 * um contador de frames como proxy leve; para tempos reais o display carimba). */

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_3DS_H */

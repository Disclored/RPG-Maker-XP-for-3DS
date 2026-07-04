/* ============================================================================
 * debug_3ds.cpp  --  Implementacao do Super Debug System  [Tarefa 7]
 * ----------------------------------------------------------------------------
 * Define as variaveis globais e funcoes auxiliares declaradas em debug_3ds.h.
 * Inclui este ficheiro no build (Makefile ja apanha *.cpp em source/).
 * ============================================================================ */
#include "debug_3ds.h"

/* Contador de frame global (atualizado pelo display). */
volatile int g_dbg_frame = 0;

/* Mascara de categorias ativas. Por defeito ligamos as mais uteis para
 * diagnostico de arranque/render/input, deixando de fora as muito ruidosas
 * (POS por sprite, TEXTURE por blit) que so se ligam quando precisas. */
/* NOTA PERF: DBG_DIALOG (log [TXT] por glifo: ~4 linhas por caractere durante
 * o typewriter) e DBG_SCALE ([SCALE] blit grande: ~3-4 linhas por frame)
 * geram escrita ao SD a cada frame -> latencia mensuravel no update durante
 * dialogo. Ja' cumpriram o seu papel (caixa de dialogo resolvida), por isso
 * saem da mascara por defeito. O codigo de logging permanece: reativa com
 *   dbg_set_mask(dbg_get_mask() | DBG_DIALOG | DBG_SCALE); */
/* ── PERFIS DE LOGGING ────────────────────────────────────────────────────────
 * O log escreve no cartao SD a cada linha. Num ARM11 a 268 MHz isso CUSTA FPS
 * reais (formatar string + escrita ao SD). Por isso a mascara default e' o
 * "perfil de JOGO": mantem marcos de arranque, plugins, fases, FPS, GC, erros e
 * a caca-setas ([SHEET?], que e' incondicional), mas DEIXA DE FORA o que escreve
 * por-frame/por-tecla. O codigo de logging fica todo la' -- reativa-se quando
 * for preciso diagnosticar, sem recompilar:
 *
 *   PERFIL DE JOGO (default, rapido)      -> a mascara abaixo.
 *   DIAGNOSTICO DE INPUT (por-tecla)      -> dbg_set_mask(dbg_get_mask() | DBG_INPUT);
 *   CENSO DE MEMORIA (bloco [SUPER]/120f) -> dbg_set_mask(dbg_get_mask() | DBG_SUPER);
 *   CAIXA DE DIALOGO (por-glifo)          -> dbg_set_mask(dbg_get_mask() | DBG_DIALOG | DBG_SCALE);
 *
 * NOTA: DBG_INPUT saiu do default -- o "[INPUT|f.. tecla detectada/trigger?]"
 * disparava POR TECLA (enchia o log ao segurar um botao no dialogo). O input
 * funciona; o log so' servia para diagnosticar o latch, ja' resolvido.
 * DBG_SUPER tambem fica de fora: a memoria esta' PROVADA como resolvida
 * (falhas_tex=0, sem OOM), logo o censo por-intervalo ja' cumpriu a missao. */
uint32_t g_dbg_mask =
    DBG_DISPLAY | DBG_RENDER | DBG_BITMAP | DBG_VRAM |
    DBG_CACHE | DBG_COMPILE | DBG_TIMING | DBG_RESOURCE |
    DBG_STATE | DBG_ERROR | DBG_MARSHAL | DBG_AUDIO;

const char* dbg_cat_name(uint32_t cat) {
    switch (cat) {
        case DBG_INPUT:    return "INPUT";
        case DBG_DISPLAY:  return "DISPLAY";
        case DBG_RENDER:   return "RENDER";
        case DBG_BITMAP:   return "BITMAP";
        case DBG_TEXTURE:  return "TEX";
        case DBG_VRAM:     return "VRAM";
        case DBG_SCALE:    return "SCALE";
        case DBG_POS:      return "POS";
        case DBG_SPRITE:   return "SPRITE";
        case DBG_CACHE:    return "CACHE";
        case DBG_COMPILE:  return "COMPILE";
        case DBG_TIMING:   return "TIMING";
        case DBG_RESOURCE: return "RESOURCE";
        case DBG_STATE:    return "STATE";
        case DBG_ERROR:    return "ERROR";
        case DBG_MARSHAL:  return "MARSHAL";
        case DBG_AUDIO:    return "AUDIO";
        case DBG_DIALOG:   return "DIALOG";
        case DBG_SUPER:    return "SUPER";
        default:           return "DBG";
    }
}

void dbg_set_mask(uint32_t mask) { g_dbg_mask = mask; }
uint32_t dbg_get_mask(void)      { return g_dbg_mask; }
void dbg_set_frame(int frame)    {
    g_dbg_frame = frame;
    /* [TAIL-FLUSH] flush periodico (1 por ~90 frames, barato) para o FIM do log chegar ao
     * disco apesar do throttle de 256 escritas do DBGLOG/dbg_flush. Sem isto, ao fechar o
     * jogo perdiam-se as ultimas ~256 linhas (menu/Gilbert de fim de sessao). Nao afeta FPS. */
    if (g_dbglog && frame >= 0 && (frame % 90) == 0) fflush(g_dbglog);
}

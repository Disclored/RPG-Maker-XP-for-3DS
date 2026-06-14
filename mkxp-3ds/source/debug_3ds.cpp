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
uint32_t g_dbg_mask =
    DBG_INPUT | DBG_DISPLAY | DBG_RENDER | DBG_BITMAP | DBG_VRAM |
    DBG_SCALE | DBG_CACHE | DBG_COMPILE | DBG_TIMING | DBG_RESOURCE |
    DBG_STATE | DBG_ERROR | DBG_MARSHAL | DBG_AUDIO | DBG_DIALOG;

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
        default:           return "DBG";
    }
}

void dbg_set_mask(uint32_t mask) { g_dbg_mask = mask; }
uint32_t dbg_get_mask(void)      { return g_dbg_mask; }
void dbg_set_frame(int frame)    { g_dbg_frame = frame; }

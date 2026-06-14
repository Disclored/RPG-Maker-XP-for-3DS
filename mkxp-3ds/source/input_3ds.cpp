#include "input_3ds.h"
#include "debug_3ds.h"
#include <string.h>

struct BMap { u32 btn; RmxpKey key; };
static const BMap MAP[] = {
    {KEY_DUP,    RMXP_UP},    {KEY_DDOWN,  RMXP_DOWN},
    {KEY_DLEFT,  RMXP_LEFT},  {KEY_DRIGHT, RMXP_RIGHT},
    {KEY_A,      RMXP_C},
    {KEY_B,      RMXP_X},
    {KEY_X,      RMXP_Z},
    {KEY_Y,      RMXP_A},
    {KEY_L,      RMXP_L},     {KEY_R,      RMXP_R},
    {KEY_START,  RMXP_F5},    {KEY_SELECT, RMXP_SHIFT},
};
static const int MSIZ = (int)(sizeof(MAP)/sizeof(MAP[0]));

/* ============================================================================
 * CIRCLE PAD (stick analogico) -- deteccao de FLANCO com deadzone + histerese.
 * ----------------------------------------------------------------------------
 * BUG ANTIGO: o circle pad marcava pressed[] por NIVEL (c.dy > 40) e NUNCA
 * marcava released[]. Resultado: ao tocar no stick (ou com drift/ruido perto
 * do centro) a direcao ficava "carregada PARA SEMPRE" -- nunca havia um
 * released[] para a limpar. No menu, com a logica de repeat?, isso fazia o
 * cursor andar sozinho; e se o stick oscilava perto do centro marcava CIMA e
 * BAIXO ao mesmo tempo -> cursor a subir e descer sem parar.
 *
 * CORRECCAO: emitir pressed[] UMA vez quando o eixo passa o limiar de pressao
 * (CPAD_ON) e released[] UMA vez quando volta abaixo do limiar de soltura
 * (CPAD_OFF). O intervalo morto entre os dois (histerese) absorve drift/ruido
 * e evita o flicker no limiar. O D-pad NAO muda (ja usa flancos hidKeysDown/Up).
 *
 * Os limiares sao conservadores (o stick tem alcance ~+-156). Se o teu circle
 * pad tiver drift acima de CPAD_OFF, o log "[INPUT] cpad ... mag=" mostra o
 * valor real e ajustamos. ========================================================================= */
#define CPAD_ON   55   /* empurrar a magnitude > 55 para REGISTAR a direcao */
#define CPAD_OFF  35   /* voltar a magnitude < 35 para SOLTAR (histerese)    */

static bool s_cp_up = false, s_cp_down = false, s_cp_left = false, s_cp_right = false;

/* Trata um eixo do circle pad. `mag` e' a magnitude JA' no sentido desta
 * direcao (ex: para DOWN passamos -c.dy). Emite press/release no flanco e
 * loga a transicao (com o valor cru) para diagnostico. */
static inline void cpad_axis(bool* pressed, bool* released, int n,
                             int key, bool* state, int mag, const char* name) {
    if (key >= n) return;
    if (!*state && mag > CPAD_ON) {
        pressed[key] = true; *state = true;
        DBG(DBG_INPUT, "cpad %s ON  (mag=%d)", name, mag);
    } else if (*state && mag < CPAD_OFF) {
        released[key] = true; *state = false;
        DBG(DBG_INPUT, "cpad %s OFF (mag=%d)", name, mag);
    }
}

void input_3ds_poll(bool* pressed, bool* released, int n) {
    hidScanInput();
    u32 dn = hidKeysDown(), up = hidKeysUp();
    for (int i = 0; i < MSIZ; i++) {
        int k = (int)MAP[i].key;
        if (k < 0 || k >= n) continue;
        if (dn & MAP[i].btn) pressed[k]  = true;
        if (up & MAP[i].btn) released[k] = true;
    }

    circlePosition c; hidCircleRead(&c);
    /* edge-detect cada direcao COM soltura (mag = sentido do eixo) */
    cpad_axis(pressed, released, n, RMXP_UP,    &s_cp_up,     (int)c.dy, "UP");
    cpad_axis(pressed, released, n, RMXP_DOWN,  &s_cp_down,  -(int)c.dy, "DOWN");
    cpad_axis(pressed, released, n, RMXP_LEFT,  &s_cp_left,  -(int)c.dx, "LEFT");
    cpad_axis(pressed, released, n, RMXP_RIGHT, &s_cp_right,  (int)c.dx, "RIGHT");
}

bool input_3ds_should_quit() { return !aptMainLoop(); }

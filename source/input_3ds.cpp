#include "input_3ds.h"
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
    if (RMXP_UP    < n && c.dy >  40) pressed[RMXP_UP]    = true;
    if (RMXP_DOWN  < n && c.dy < -40) pressed[RMXP_DOWN]  = true;
    if (RMXP_LEFT  < n && c.dx < -40) pressed[RMXP_LEFT]  = true;
    if (RMXP_RIGHT < n && c.dx >  40) pressed[RMXP_RIGHT] = true;
}

bool input_3ds_should_quit() { return !aptMainLoop(); }

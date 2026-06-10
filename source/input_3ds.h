#pragma once
#include <3ds.h>

enum RmxpKey {
    RMXP_DOWN=2, RMXP_LEFT=4, RMXP_RIGHT=6, RMXP_UP=8,
    RMXP_A=11, RMXP_B=12, RMXP_C=13, RMXP_X=14,
    RMXP_Y=15,  RMXP_Z=16, RMXP_L=17, RMXP_R=18,
    RMXP_SHIFT=21, RMXP_F5=25,
    RMXP_KEY_COUNT=32
};

void input_3ds_poll(bool* pressed, bool* released, int key_count);
bool input_3ds_should_quit();

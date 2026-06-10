#include "platform_3ds.h"
#include <3ds.h>
#include <stdio.h>

void platform3ds_init() {}

const char* platform3ds_get_config_file() {
    return "sdmc:/mkxp/game/mkxp.json";
}

void platform3ds_print(const char* msg) {
    printf("%s\n", msg);
}

bool platform3ds_should_quit() {
    return !aptMainLoop();
}

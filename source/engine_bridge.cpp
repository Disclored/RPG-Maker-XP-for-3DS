#include "display_3ds.h"
#include "input_3ds.h"
#include "platform_3ds.h"
#include "binding_3ds.h"
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>

int mkxp_3ds_run(const char* game_path) {
    platform3ds_print("engine_bridge: display init");
    display_3ds_init();
    platform3ds_print("engine_bridge: calling binding");
    printf("  game_path = %s\n", game_path);

    int result = binding_3ds_run(game_path);

    platform3ds_print("engine_bridge: binding returned");
    return result;
}

void mkxp_3ds_cleanup() {
    binding_3ds_cleanup();
    platform3ds_print("engine_bridge: cleanup");
}
